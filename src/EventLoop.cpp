#include "lift/EventLoop.h"

#include <curl/multi.h>

#include <thread>
#include <chrono>

using namespace std::chrono_literals;

namespace lift
{

class CurlContext
{
public:
    CurlContext(
        EventLoop& event_loop,
        uv_loop_t* uv_loop,
        curl_socket_t sock_fd
    )
        : m_event_loop(event_loop),
          m_sock_fd(sock_fd)
    {
        uv_poll_init_socket(uv_loop, &m_poll_handle, m_sock_fd);
        m_poll_handle.data = this;
    }

    CurlContext(const CurlContext& copy) = delete;
    CurlContext(CurlContext&& move) = default;
    auto operator=(const CurlContext& assign) = delete;

    auto Close()
    {
        uv_poll_stop(&m_poll_handle);
        /**
         * uv requires us to jump through a few hoops before we can delete ourselves.
         */
        uv_close(reinterpret_cast<uv_handle_t*>(&m_poll_handle), CurlContext::on_close);
    }

    auto GetEventLoop() -> EventLoop&
    {
        return m_event_loop;
    }

    auto GetPollHandle() -> uv_poll_t*
    {
        return &m_poll_handle;
    }
    auto GetCurlSocket() -> curl_socket_t
    {
        return m_sock_fd;
    }

private:
    EventLoop& m_event_loop;
    uv_poll_t m_poll_handle;
    curl_socket_t m_sock_fd;

    static auto on_close(uv_handle_t* handle) -> void
    {
        auto* curl_context = static_cast<CurlContext*>(handle->data);
        /**
         * uv has signaled that it is finished with the m_poll_handle,
         * we can now safely delete 'this'.
         */
        delete curl_context;
    }
};

auto curl_start_timeout(
    CURLM* cmh,
    long timeout_ms,
    void* user_data
) -> void;

auto curl_handle_socket_actions(
    CURL* curl,
    curl_socket_t socket,
    int action,
    void* user_data,
    void* socketp
) -> int;

auto uv_close_callback(
    uv_handle_t* handle
) -> void;

auto on_uv_timeout_callback(
    uv_timer_t* handle,
    int status
) -> void;

auto on_uv_curl_perform_callback(
    uv_poll_t* req,
    int status,
    int events
) -> void;

auto requests_accept_async(
    uv_async_t* async,
    int status
) -> void;

EventLoop::EventLoop(
    std::unique_ptr<IRequestCb> request_callback
)
    : m_is_running(false),
      m_active_request_count(0),
      m_request_callback(std::move(request_callback)),
      m_loop(uv_loop_new()),
      m_cmh(curl_multi_init()),
      m_async_closed(false),
      m_timeout_timer_closed(false)
{
    uv_async_init(m_loop, &m_async, requests_accept_async);
    m_async.data = this;

    uv_timer_init(m_loop, &m_timeout_timer);
    m_timeout_timer.data = this;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
    curl_multi_setopt(m_cmh, CURLMOPT_SOCKETFUNCTION, curl_handle_socket_actions);
    curl_multi_setopt(m_cmh, CURLMOPT_SOCKETDATA,     this);
    curl_multi_setopt(m_cmh, CURLMOPT_TIMERFUNCTION,  curl_start_timeout);
    curl_multi_setopt(m_cmh, CURLMOPT_TIMERDATA,      this);
#pragma clang diagnostic pop

    m_background_thread = std::thread([this] { run(); });

    /**
     * Wait for the thread to spin-up and run the event loop,
     * this means when the constructor returns the user can start adding requests
     * immediately without waiting.
     */
    while(!IsRunning())
    {
        std::this_thread::sleep_for(1ms);
    }
}

EventLoop::~EventLoop()
{
    // Curl needs to be cleaned up first or the uv_loop close callbacks will fail.
    curl_multi_cleanup(m_cmh);
    uv_loop_delete(m_loop);
}

auto EventLoop::IsRunning() -> bool
{
    return m_is_running;
}

auto EventLoop::GetActiveRequestCount() const -> uint64_t
{
    return m_active_request_count;
}

auto EventLoop::Stop() -> void
{
    uv_timer_stop(&m_timeout_timer);

    /**
     * Close the timer and async handles.  The callback will mark each item
     * as properly closed before stop is called on the loop.
     */
    uv_close(reinterpret_cast<uv_handle_t*>(&m_timeout_timer), uv_close_callback);
    uv_close(reinterpret_cast<uv_handle_t*>(&m_async),         uv_close_callback);

    /**
     * Fake a request to the event loop so it will wake up if it is in a blocking wait.
     * This is important or the uv_run() call can block indefinitely.
     */
    uv_async_send(&m_async);

    while(!m_timeout_timer_closed && !m_async_closed)
    {
        std::this_thread::sleep_for(1ms);
    }
    uv_stop(m_loop);

    m_background_thread.join();
}

auto EventLoop::GetRequestPool() -> RequestPool&
{
    return m_request_pool;
}

auto EventLoop::AddRequest(
    Request request
) -> void
{
    // We'll prepare now since it won't block the event loop thread.
    request->prepareForPerform();
    {
        std::lock_guard<std::mutex> guard(m_pending_requests_lock);
        m_pending_requests.emplace_back(std::move(request));
    }
    uv_async_send(&m_async);
}

auto EventLoop::GetRequestCallback() -> IRequestCb&
{
    return *m_request_callback;
}

auto EventLoop::GetRequestCallback() const -> const IRequestCb&
{
    return *m_request_callback;
}

auto EventLoop::run() -> void
{
    m_is_running = true;
    uv_run(m_loop, UV_RUN_DEFAULT);
    m_is_running = false;
}

auto EventLoop::checkActions() -> void
{
    checkActions(CURL_SOCKET_TIMEOUT, 0);
}

auto EventLoop::checkActions(curl_socket_t socket, int event_bitmask) -> void
{
    int running_handles = 0;
    CURLMcode curl_code = CURLM_OK;
    do
    {
        curl_code = curl_multi_socket_action(m_cmh, socket, event_bitmask, &running_handles);
    } while(curl_code == CURLM_CALL_MULTI_PERFORM);

    CURLMsg* message = nullptr;
    int msgs_left = 0;

    while((message = curl_multi_info_read(m_cmh, &msgs_left)))
    {
        if(message->msg == CURLMSG_DONE)
        {
            /**
             * Per docs do not use 'message' after calling curl_multi_remove_handle.
             */
            auto easy_handle = message->easy_handle;
            auto easy_result = message->data.result;

            RequestHandle* raw_request_handle_ptr = nullptr;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdisabled-macro-expansion"
            curl_easy_getinfo(easy_handle, CURLINFO_PRIVATE, &raw_request_handle_ptr);
#pragma clang diagnostic pop
            curl_multi_remove_handle(m_cmh, easy_handle);

            /**
             * Encapsulate the RequestHandle into  Request proxy object for the client
             * OnComplete() callback.  Curl has kept the memory alive for us.
             */
            Request request(&m_request_pool, std::unique_ptr<RequestHandle>(raw_request_handle_ptr));
            request->setRequestStatus(easy_result);
            m_request_callback->OnComplete(std::move(request));
            --m_active_request_count;
        }
    }
}

auto curl_start_timeout(
    CURLM* /*cmh*/,
    long timeout_ms,
    void* user_data
) -> void
{
    auto* event_loop = static_cast<EventLoop*>(user_data);

    // Stop the current timer regardless.
    uv_timer_stop(&event_loop->m_timeout_timer);

    if(timeout_ms > 0)
    {
        uv_timer_start(
            &event_loop->m_timeout_timer,
            on_uv_timeout_callback,
            static_cast<uint64_t>(timeout_ms),
            0
        );
    }
    else if(timeout_ms == 0)
    {
        event_loop->checkActions();
    }
}

auto curl_handle_socket_actions(
    CURL* /*curl*/,
    curl_socket_t socket,
    int action,
    void* user_data,
    void* socketp
) -> int
{
    auto* event_loop = static_cast<EventLoop*>(user_data);

    CurlContext* curl_context = nullptr;
    if(action == CURL_POLL_IN || action == CURL_POLL_OUT)
    {
        if(socketp != nullptr)
        {
            // existing request
            curl_context = static_cast<CurlContext*>(socketp);
        }
        else
        {
            // new request
            curl_context = new CurlContext(*event_loop, event_loop->m_loop, socket);
            curl_multi_assign(event_loop->m_cmh, socket, static_cast<void*>(curl_context));
        }
    }

    switch(action)
    {
        case CURL_POLL_IN:
            uv_poll_start(curl_context->GetPollHandle(), UV_READABLE, on_uv_curl_perform_callback);
            break;
        case CURL_POLL_OUT:
            uv_poll_start(curl_context->GetPollHandle(), UV_WRITABLE, on_uv_curl_perform_callback);
            break;
        case CURL_POLL_REMOVE:
            if(socketp != nullptr)
            {
                curl_context = static_cast<CurlContext*>(socketp);
                curl_context->Close(); // signal this handle is done
                curl_multi_assign(event_loop->m_cmh, socket, nullptr);
            }
            break;
        default:
            break;
    }

    return 0;
}

auto uv_close_callback(uv_handle_t* handle) -> void
{
    auto event_loop = static_cast<EventLoop*>(handle->data);;
    if(handle == reinterpret_cast<uv_handle_t*>(&event_loop->m_async))
    {
        event_loop->m_async_closed = true;
    }
    else if(handle == reinterpret_cast<uv_handle_t*>(&event_loop->m_timeout_timer))
    {
        event_loop->m_timeout_timer_closed = true;
    }
}

auto on_uv_timeout_callback(
    uv_timer_t* handle,
    int /*status*/
) -> void
{
    auto* event_loop = static_cast<EventLoop*>(handle->data);
    event_loop->checkActions();
}

auto on_uv_curl_perform_callback(
    uv_poll_t* req,
    int status,
    int events
) -> void
{
    auto* curl_context = static_cast<CurlContext*>(req->data);
    auto& event_loop = curl_context->GetEventLoop();

    int action = 0;
    if(status < 0)
    {
        action = CURL_CSELECT_ERR;
    }
    if(!status && (events & UV_READABLE))
    {
        action |= CURL_CSELECT_IN;
    }
    if(!status && (events & UV_WRITABLE))
    {
        action |= CURL_CSELECT_OUT;
    }

    event_loop.checkActions(curl_context->GetCurlSocket(), action);
}

auto requests_accept_async(
    uv_async_t* handle,
    int /*status*/
) -> void
{
    auto* event_loop = static_cast<EventLoop*>(handle->data);

    // Lock scope.
    {
        std::lock_guard<std::mutex> guard(event_loop->m_pending_requests_lock);

        for(auto& request : event_loop->m_pending_requests)
        {
            /**
             * Drop the unique_ptr safety around the RequestHandle while it is being
             * processed by curl.  When curl is finished completing the request
             * it will be put back into a Request object for the client to use.
             */
            auto* raw_request_handle_ptr = request.m_request_handle.release();

            curl_multi_add_handle(event_loop->m_cmh, raw_request_handle_ptr->m_curl_handle);
        }

        event_loop->m_active_request_count += event_loop->m_pending_requests.size();
        event_loop->m_pending_requests.clear();
    }
}

} // lift
