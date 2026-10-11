/* Included by runtime_pad.c; motion samples are published by SDL's event thread. */
#include <math.h>
static pthread_mutex_t motion_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    SDL_JoystickID device;
    int enabled, accel, gyro, seen[2], warned;
    uint64_t received[2], gyro_time;
    float acceleration[3], velocity[3], orientation[4];
} motion;
static int motion_watching;

static void motion_reset_locked(void) {
    memset(motion.orientation,0,sizeof(motion.orientation));
    motion.orientation[3]=1;
    motion.gyro_time=0;
}
static bool SDLCALL motion_event(void *unused,SDL_Event *event) {
    (void)unused;
    if (event->type!=SDL_EVENT_GAMEPAD_SENSOR_UPDATE) return true;
    SDL_GamepadSensorEvent *s=&event->gsensor;
    if (s->sensor!=SDL_SENSOR_ACCEL && s->sensor!=SDL_SENSOR_GYRO) return true;
    pthread_mutex_lock(&motion_lock);
    if (!motion.enabled || s->which!=motion.device) goto done;
    int gyro=s->sensor==SDL_SENSOR_GYRO;
    if (motion.seen[gyro] && s->timestamp<motion.received[gyro]) goto done;
    if (gyro && motion.gyro_time && s->sensor_timestamp<=motion.gyro_time) goto done;
    for (int i=0;i<3;++i) if (!isfinite(s->data[i])) goto done;
    motion.seen[gyro]=1; motion.received[gyro]=s->timestamp;
    if (!gyro) {
        for (int i=0;i<3;++i) motion.acceleration[i]=s->data[i]/SDL_STANDARD_GRAVITY;
    } else {
        memcpy(motion.velocity,s->data,sizeof(motion.velocity));
        uint64_t stamp=s->sensor_timestamp;
        if (motion.gyro_time && stamp>motion.gyro_time && stamp-motion.gyro_time<=250000000) {
            double dt=(stamp-motion.gyro_time)*1e-9;
            double x=s->data[0],y=s->data[1],z=s->data[2];
            double speed=sqrt(x*x+y*y+z*z);
            if (speed>0) {
                double factor=sin(speed*dt/2)/speed;
                x*=factor; y*=factor; z*=factor;
                double w=cos(speed*dt/2);
                float *q=motion.orientation;
                double next[4]={q[3]*x+q[0]*w+q[1]*z-q[2]*y,
                    q[3]*y-q[0]*z+q[1]*w+q[2]*x,
                    q[3]*z+q[0]*y-q[1]*x+q[2]*w,
                    q[3]*w-q[0]*x-q[1]*y-q[2]*z};
                double norm=sqrt(next[0]*next[0]+next[1]*next[1]+next[2]*next[2]+next[3]*next[3]);
                for (int i=0;i<4;++i) q[i]=(float)(next[i]/norm);
            }
        }
        /* Repeated/backwards timestamps never integrate twice. A gap starts a new interval. */
        if (stamp>motion.gyro_time) motion.gyro_time=stamp;
    }
done:
    pthread_mutex_unlock(&motion_lock);
    return true;
}
static void motion_connect(SDL_Gamepad *g) {
    if (!motion_watching) {
        if (!SDL_AddEventWatch(motion_event,NULL)) { pad_failure("SDL motion event watch"); return; }
        motion_watching=1;
    }
    SDL_JoystickID device=SDL_GetGamepadID(g);
    int accel=SDL_GamepadHasSensor(g,SDL_SENSOR_ACCEL);
    int gyro=SDL_GamepadHasSensor(g,SDL_SENSOR_GYRO);
    pthread_mutex_lock(&motion_lock);
    memset(&motion,0,sizeof(motion));
    motion.device=device; motion.enabled=1;
    motion.accel=accel; motion.gyro=gyro;
    motion_reset_locked();
    pthread_mutex_unlock(&motion_lock);
    if (accel && !SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_ACCEL,true)) pad_failure("SDL accelerometer enable");
    if (gyro && !SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_GYRO,true)) pad_failure("SDL gyroscope enable");
    printf("Runtime: gamepad motion accel=%d gyro=%d; acceleration in g, angular velocity rad/s; orientation is gyro-integrated, PS4 fusion unqualified\n",accel,gyro);
}
static void motion_sample(PadData *d) {
    uint64_t now=SDL_GetTicksNS();
    pthread_mutex_lock(&motion_lock);
    if (motion.enabled) {
        for (int i=0;i<2;++i) {
            int fresh=motion.seen[i] && now>=motion.received[i] && now-motion.received[i]<=250000000;
            if (fresh) memcpy(i ? d->angular_velocity : d->acceleration,
                              i ? motion.velocity : motion.acceleration,3*sizeof(float));
            else if (motion.seen[i] && !motion.warned) {
                motion.warned=1;
                fprintf(stderr,"CONTROLLER MOTION FAILURE: sensor sample older than 250 ms; stale vector suppressed, orientation held\n");
            }
        }
        memcpy(d->orientation,motion.orientation,sizeof(motion.orientation));
    }
    pthread_mutex_unlock(&motion_lock);
}
static ABI int32_t pad_reset_orientation(int32_t handle) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    pthread_mutex_lock(&motion_lock); motion_reset_locked(); pthread_mutex_unlock(&motion_lock);
    return 0;
}
static ABI int32_t pad_motion_enable(int32_t handle,uint8_t enabled) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    pthread_mutex_lock(&lock);
    SDL_Gamepad *g=current_gamepad();
    pthread_mutex_lock(&motion_lock);
    motion.enabled=enabled!=0; motion.seen[0]=motion.seen[1]=0; motion.gyro_time=0;
    int accel=motion.accel,gyro=motion.gyro;
    pthread_mutex_unlock(&motion_lock);
    if (g && accel && !SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_ACCEL,enabled!=0)) pad_failure("SDL accelerometer state");
    if (g && gyro && !SDL_SetGamepadSensorEnabled(g,SDL_SENSOR_GYRO,enabled!=0)) pad_failure("SDL gyroscope state");
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t pad_motion_filter(int32_t handle,uint8_t enabled) {
    if (handle!=PAD_HANDLE || !opened) return ERR_INVALID_HANDLE;
    if (enabled) fprintf(stderr,"CONTROLLER MOTION LIMIT: PS4 deadband/tilt filtering is unqualified; raw SDL motion remains active\n");
    return 0;
}
