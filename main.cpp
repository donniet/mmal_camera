#include <iostream>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/util/mmal_util.h>
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

struct mmal_connection_enabled {
    MMAL_CONNECTION_T * p;

    mmal_connection_enabled(MMAL_CONNECTION_T * p) : p(p) {
        std::cerr << "enabling connection" << std::endl;
        if (mmal_connection_enable(p) != MMAL_SUCCESS) {
            throw std::logic_error("failed to enable connection");
        }
    }
    ~mmal_connection_enabled() {
        mmal_connection_disable(p);
    }
};

struct component_enabler {
private:
    MMAL_COMPONENT_T * comp;
public:
    component_enabler(component_enabler const &) = delete;
    component_enabler(component_enabler && rhs) {
        comp = rhs.comp;
        rhs.comp = nullptr;
    }
    component_enabler(MMAL_COMPONENT_T * comp) : comp(comp) {
        MMAL_STATUS_T status = mmal_component_enable(comp);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable component");
    }
    ~component_enabler() {
        if (comp != nullptr) mmal_component_disable(comp);
    }
};

class Component {
    friend struct component_enabler;
protected:
    mmal_unique_ptr<MMAL_COMPONENT_T> comp;

    Component(std::string const & name) : comp(name) {}

public:
    component_enabler enable_scoped() {
        return component_enabler(comp);
    }
};

class Camera : public Component {
private:
    uint32_t width;
    uint32_t height;
    float fps;
    int num;

    static const int camera_preview_port = 0;
    static const int camera_video_port = 1;
    static const int camera_capture_port = 2;

    static void camera_control_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T * buffer) {
        std::cerr << "camera control callback, cmd=" << std::ios::hex << buffer->cmd << std::endl;

        if (buffer->cmd == MMAL_EVENT_PARAMETER_CHANGED) {
            MMAL_EVENT_PARAMETER_CHANGED_T *param = (MMAL_EVENT_PARAMETER_CHANGED_T *)buffer->data;
            switch (param->hdr.id) {
            case MMAL_PARAMETER_CAMERA_SETTINGS: {
                MMAL_PARAMETER_CAMERA_SETTINGS_T *settings = (MMAL_PARAMETER_CAMERA_SETTINGS_T*)param;
                fprintf(stderr, "Exposure now %u, analog gain %u/%u, digital gain %u/%u",
                                settings->exposure,
                                settings->analog_gain.num, settings->analog_gain.den,
                                settings->digital_gain.num, settings->digital_gain.den);
                fprintf(stderr, "AWB R=%u/%u, B=%u/%u",
                                settings->awb_red_gain.num, settings->awb_red_gain.den,
                                settings->awb_blue_gain.num, settings->awb_blue_gain.den);
            }
            break;
            }
        }
        else if (buffer->cmd == MMAL_EVENT_ERROR) {
            fprintf(stderr, "No data received from sensor. Check all connections, including the Sunny one on the camera board");
        }
        else {
            fprintf(stderr, "Received unexpected camera control callback event, 0x%08x", buffer->cmd);
        }

        mmal_buffer_header_release(buffer);
    }

public:
    Camera(uint32_t w, uint32_t h, float fps, int num = 0) : 
        Component(MMAL_COMPONENT_DEFAULT_CAMERA), width(w), height(h), fps(fps), num(num) {
        MMAL_PARAMETER_INT32_T camera_num = {
            {MMAL_PARAMETER_CAMERA_NUM, sizeof(camera_num)},
            num
        };

        MMAL_STATUS_T status;

        status = mmal_port_parameter_set(comp->control, &camera_num.hdr);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not set camera number");
        if (comp->output_num == 0) throw std::logic_error("camera has no outputs");

        status = mmal_port_enable(comp->control, camera_control_callback);

        check_camera_model();
        configure(w, h, fps);
    }

    void check_camera_model() {
        MMAL_STATUS_T status;
        mmal_unique_ptr<MMAL_COMPONENT_T> camera_info(MMAL_COMPONENT_DEFAULT_CAMERA_INFO);

        MMAL_PARAMETER_CAMERA_INFO_T param;
        param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
        param.hdr.size = sizeof(param); // deliberately undersize
        status = mmal_port_parameter_get(camera_info->control, &param.hdr);
        if (status != MMAL_SUCCESS) throw std::logic_error("error getting camera info");
        if (param.num_cameras < uint32_t(num)) throw std::logic_error("invalid camera number");

        if (!strncmp(param.cameras[num].camera_name, "toshh2c", 7)) 
            throw std::logic_error("invalid camera name");

        std::cerr << "camera name: " << param.cameras[num].camera_name << std::endl;
    }

    void configure(uint32_t width, uint32_t height, float fps) {
        MMAL_PARAMETER_CAMERA_CONFIG_T config = {
            { MMAL_PARAMETER_CAMERA_CONFIG, sizeof(config)},
            .max_stills_w = width,
            .max_stills_h = height,
            .stills_yuv422 = 0,
            .one_shot_stills = 0,
            .max_preview_video_w = width,
            .max_preview_video_h = height,
            .num_preview_video_frames = 3 + uint32_t(std::max(0, int((fps-30)/10))),
            .stills_capture_circular_buffer_height = 0,
            .fast_preview_resume = 0,
            .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RAW_STC
        };
        mmal_port_parameter_set(comp->control, &config.hdr);

        
        MMAL_ES_FORMAT_T *format;

        format = preview_port()->format;

        format->encoding = MMAL_ENCODING_OPAQUE;
        format->encoding_variant = MMAL_ENCODING_I420;

        format->es->video.width = VCOS_ALIGN_UP(width, 32);
        format->es->video.height = VCOS_ALIGN_UP(height, 16);
        format->es->video.crop.x = 0;
        format->es->video.crop.y = 0;
        format->es->video.crop.width = width;
        format->es->video.crop.height = height;
        format->es->video.frame_rate.num = int32_t(fps * 1000);
        format->es->video.frame_rate.den = 1000;

        MMAL_STATUS_T status;

        status = mmal_port_format_commit(preview_port());
        if (status != MMAL_SUCCESS) throw std::logic_error("could not commit preview port format");
        
    }

    MMAL_PORT_T * preview_port() {
        return comp->output[camera_preview_port];
    }
    MMAL_PORT_T * video_port() {
        return comp->output[camera_video_port];
    }
    MMAL_PORT_T * capture_port() {
        return comp->output[camera_capture_port];
    }
};

class Encoder : public Component {
private:
    uint32_t bitrate;
    MMAL_POOL_T * pool;


    static void control_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T * buffer) {
        mmal_buffer_header_release(buffer);
    }
public:
    Encoder(uint32_t bitrate = 25000000) : Component(MMAL_COMPONENT_DEFAULT_VIDEO_ENCODER), bitrate(bitrate) {
        MMAL_STATUS_T status;
        MMAL_PORT_T * in, * out;

        if (comp->input_num == 0 || comp->output_num == 0)
            throw std::logic_error("encoder does not have any input/output ports");

        comp->control->userdata = reinterpret_cast<struct MMAL_PORT_USERDATA_T*>(this);
        status = mmal_port_enable(comp->control, control_callback);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable port on video encoder");
        

        in = comp->input[0];
        out = comp->output[0];

        mmal_format_copy(out->format, in->format);

        out->format->encoding = MMAL_ENCODING_H264;
        out->format->bitrate = bitrate;
        out->buffer_size = out->buffer_num_recommended;
        out->buffer_num = out->buffer_num_recommended;
        out->format->es->video.frame_rate.num = 0;
        out->format->es->video.frame_rate.den = 1;

        status = mmal_port_format_commit(out);
        if (status != MMAL_SUCCESS) throw std::logic_error("unable to set format on encoder output port");

        // set slices
        // set quantization
        // set level
        // set immutable input
        status = mmal_port_parameter_set_boolean(out, MMAL_PARAMETER_VIDEO_ENCODE_INLINE_HEADER, true);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not set inline headers");

        status = mmal_port_parameter_set_boolean(out, MMAL_PARAMETER_VIDEO_ENCODE_SPS_TIMING, true);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not set inline headers");

        // set motion vectors
        // set refresh type

        // status = mmal_component_enable(comp);
        // if (status != MMAL_SUCCESS) throw std::logic_error("could not enable video_encoder");

        // pool = mmal_port_pool_create(out, out->buffer_num, out->buffer_size); 
        // if (pool == NULL) throw std::logic_error("could not create output pool");     
    }

    MMAL_PORT_T * input() { return comp->input[0]; }
    MMAL_PORT_T * output() { return comp->output[0]; }

    ~Encoder() {
        mmal_port_pool_destroy(output(), pool);
    }
};

class Context {
private:
    std::mutex m;
    std::condition_variable ready;

    Camera camera;
    Encoder video_encoder;
    mmal_unique_ptr<MMAL_CONNECTION_T> camera_to_video_encoder;

    MMAL_STATUS_T status;
    bool eos;

    static void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
        Context * ctx = reinterpret_cast<Context*>(port->userdata);

        std::cerr << "output callback" << std::endl;
        // mmal_queue_put(ctx->queue, buffer);

        ctx->ready.notify_all();
    }

    static void control_callback(MMAL_PORT_T * port, MMAL_BUFFER_HEADER_T *buffer) {
        Context * ctx = reinterpret_cast<Context*>(port->userdata);

        std::cerr << "control callback" << std::endl;

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

        std::cerr << "connection callback" << std::endl;

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

        std::cerr << "step" << std::endl;

        if (status != MMAL_SUCCESS) {
            return false;
        }
        if (eos) return false;

        // MMAL_BUFFER_HEADER_T * buf;
        // MMAL_STATUS_T status;

        // if (!(camera_to_video_encoder->flags & MMAL_CONNECTION_FLAG_TUNNELLING)) {
        //     while((buf = mmal_queue_get(camera_to_video_encoder->pool->queue)) != nullptr) {
        //         status = mmal_port_send_buffer(camera_to_video_encoder->out, buf);
        //         if (status != MMAL_SUCCESS) throw std::logic_error("failed to send buffer");
        //     }

        //     while((buf = mmal_queue_get(camera_to_video_encoder->queue)) != nullptr) {
        //         status = mmal_port_send_buffer(camera_to_video_encoder->in, buf);
        //         if (status != MMAL_SUCCESS) throw std::logic_error("failed to queue buffer");
        //     }
        // }


        return true;
    }
public:
    Context() : 
        camera(1440, 1080, 25, 0),
        video_encoder(),
        camera_to_video_encoder(video_encoder.input(), camera.preview_port(), MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT),
        status(MMAL_SUCCESS),
        eos(false)
    { 
        MMAL_STATUS_T status;
        // MMAL_PORT_T * encoder_input = video_encoder.input();


        camera_to_video_encoder->user_data = (void*)this;
        camera_to_video_encoder->callback = connection_callback;

        status = mmal_port_enable(video_encoder.output(), output_callback);
        if (status != MMAL_SUCCESS) throw std::logic_error("could not enable encoder output port");

        // status = mmal_port_enable(camera.preview_port(), output_callback);
        // if (status != MMAL_SUCCESS) throw std::logic_error("could not enable preview port");
    }

    void start() { 
        std::cerr << "starting..." << std::endl;

        MMAL_STATUS_T status;
        status = mmal_port_parameter_set_boolean(camera.video_port(), MMAL_PARAMETER_CAPTURE, true);
        if (status != MMAL_SUCCESS) std::logic_error("error starting capture");

        mmal_connection_enabled enabled(camera_to_video_encoder);
        auto camera_enabled = camera.enable_scoped();
        auto encder_enabled = video_encoder.enable_scoped();

        while(processing_step()) { }
    }

    void stop() {
    }

    ~Context() {
    }
};


int main(int ac, char ** av) {
    bcm_host_init();

    Context ctx;

    ctx.start();

    return 0;
}

