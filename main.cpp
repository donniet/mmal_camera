#include <iostream>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/mmal_format.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_component_wrapper.h>
#include <interface/mmal/util/mmal_util_params.h>

#include <mutex>
#include <condition_variable>

template<typename T>
struct mmal_unique_ptr_base {
    T * p;

    T * operator->() { return p; }
    const T* operator->() const { return p; }
    operator T* () { return p; }
    operator const T*() const { return p; }
};

template<typename T> struct mmal_unique_ptr;

template<> 
struct mmal_unique_ptr<MMAL_COMPONENT_T> : 
    public mmal_unique_ptr_base<MMAL_COMPONENT_T>
{
    mmal_unique_ptr(std::string const & name) {
        if (mmal_component_create(name.c_str(), &p) != MMAL_SUCCESS) {
            throw std::logic_error("could not create component");
        }
    }
    ~mmal_unique_ptr() {
        mmal_component_destroy(p);
    }
};

struct mmal_connection_enabled {
    MMAL_CONNECTION_T * p;

    mmal_connection_enabled(MMAL_CONNECTION_T * p) : p(p) {
        if (mmal_connection_enable(p) != MMAL_SUCCESS) {
            throw std::logic_error("failed to enable connection");
        }
    }
    ~mmal_connection_enabled() {
        mmal_connection_disable(p);
    }
};

template<> 
struct mmal_unique_ptr<MMAL_CONNECTION_T> : 
    public mmal_unique_ptr_base<MMAL_CONNECTION_T>
{
    mmal_unique_ptr(MMAL_PORT_T * out, MMAL_PORT_T * in, uint32_t flags = 0) {
        if (mmal_connection_create(&p, out, in, flags) != MMAL_SUCCESS) {
            throw std::logic_error("could not create component");
        }
    }
    ~mmal_unique_ptr() {
        mmal_connection_destroy(p);
    }
};

class Context {
private:
    std::mutex m;
    std::condition_variable ready;

    mmal_unique_ptr<MMAL_COMPONENT_T> camera;
    mmal_unique_ptr<MMAL_COMPONENT_T> video_encoder;
    mmal_unique_ptr<MMAL_CONNECTION_T> camera_to_video_encoder;

    MMAL_STATUS_T status;
    bool eos;

    static void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
        Context * ctx = reinterpret_cast<Context*>(port->userdata);

        // mmal_queue_put(ctx->queue, buffer);

        ctx->ready.notify_all();
    }

    static void control_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T *buffer) {
        Context * ctx = reinterpret_cast<Context*>(port->userdata);

        if (buffer->cmd == MMAL_EVENT_ERROR) {
            ctx->status = *(MMAL_STATUS_T*)buffer->data;
        } else if (buffer->cmd == MMAL_EVENT_EOS) {
            ctx->eos = true;
        }

        mmal_buffer_header_release(buffer);

        ctx->ready.notify_all();
    }

    static void connection_callback(MMAL_CONNECTION_T * c) {
        Context * ctx = reinterpret_cast<Context*>(c->user_data);

        ctx->ready.notify_all();
    }

    void check_status(MMAL_STATUS_T status, std::string msg = "error") {
        if (status != MMAL_SUCCESS) {
            std::cerr << "mmal_camera: " << msg << std::endl;
            exit(1);
        }
    }

    bool processing_step() {
        std::unique_lock<std::mutex> lk(m);

        ready.wait(lk);

        if (status != MMAL_SUCCESS) {
            return false;
        }
        if (eos) return false;

        MMAL_BUFFER_HEADER_T * buf;
        MMAL_STATUS_T status;

        while((buf = mmal_queue_get(camera_to_video_encoder->pool->queue)) != nullptr) {
            status = mmal_port_send_buffer(camera_to_video_encoder->out, buf);
            if (status != MMAL_SUCCESS) throw std::logic_error("failed to send buffer");
        }

        while((buf = mmal_queue_get(camera_to_video_encoder->queue)) != nullptr) {
            status = mmal_port_send_buffer(camera_to_video_encoder->in, buf);
            if (status != MMAL_SUCCESS) throw std::logic_error("failed to queue buffer");
        }

        return true;
    }

    void processing_loop() {
        mmal_connection_enabled enabled(camera_to_video_encoder);

        while(processing_step()) { }
    }
public:
    Context() : 
        camera(MMAL_COMPONENT_DEFAULT_CAMERA),
        video_encoder(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER),
        camera_to_video_encoder(video_encoder->input[0], camera->output[0]),
        status(MMAL_SUCCESS),
        eos(false)
    { 
        MMAL_STATUS_T status;

        camera_to_video_encoder->user_data = (void*)this;
        camera_to_video_encoder->callback = connection_callback;

        camera->control->userdata = reinterpret_cast<struct MMAL_PORT_USERDATA_T*>(this);
        status = mmal_port_enable(camera->control, control_callback);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable port on camera");
        status = mmal_component_enable(camera);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable camera");

        video_encoder->control->userdata = reinterpret_cast<struct MMAL_PORT_USERDATA_T*>(this);
        status = mmal_port_enable(video_encoder->control, control_callback);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable port on video encoder");
        status = mmal_component_enable(video_encoder);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable video_encoder");

    }

    void start() { processing_loop(); }

    ~Context() {
    }
};


int main(int ac, char ** av) {
    bcm_host_init();

    Context ctx;

    ctx.start();

    return 0;
}

