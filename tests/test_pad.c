#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <SDL3/SDL.h>
static int fail_init;
static bool test_pad_init(SDL_InitFlags flags) { return fail_init ? SDL_SetError("injected gamepad initialization failure") : SDL_InitSubSystem(flags); }
#define SDL_InitSubSystem test_pad_init
#include "../src/runtime_pad.c"
#undef SDL_InitSubSystem
static bool virtual_sensors(void *user,bool enabled) { (void)user; (void)enabled; return true; }
static int fail_rumble, rumble_calls;
static Uint16 rumble_low,rumble_high;
static bool virtual_rumble(void *user,Uint16 low,Uint16 high) {
    (void)user; ++rumble_calls; rumble_low=low; rumble_high=high;
    return fail_rumble ? SDL_SetError("injected rumble failure") : true;
}

static int capture;
#ifdef __APPLE__
int32_t *runtime_errno(void) { static _Thread_local int32_t value; return &value; }
#endif
int bbgpu_overlay_captures_input(void) { return capture; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    unsetenv("BB_PAD_MOTION");
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.Rumble=virtual_rumble;
    SDL_VirtualJoystickSensorDesc sensors[2]={{.type=SDL_SENSOR_ACCEL,.rate=100},{.type=SDL_SENSOR_GYRO,.rate=100}};
    desc.nsensors=2; desc.sensors=sensors; desc.SetSensorsEnabled=virtual_sensors;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==2 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touches[0].x==1439 && data.touches[0].y==471 && data.touches[0].id==0);
    assert(data.touches[1].x==480 && data.touches[1].y==942 && data.touches[1].id==1);
#ifdef __APPLE__
    assert(!motion_watching && data.angular_velocity[0]==0 && data.orientation[3]==1);
    assert(!pad_motion_enable(1,1) && !pad_reset_orientation(1) && !pad_motion_filter(1,1));
    assert(!pad_read_state(1,&data));
    assert(!motion_watching && !motion.enabled && data.orientation[0]==0 && data.orientation[1]==0 && data.orientation[2]==0 && data.orientation[3]==1);
    assert(!SDL_GamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO));
    SDL_CloseGamepad(gamepad); gamepad=NULL;
    setenv("BB_PAD_MOTION","diagnostic",1);
    assert(!pad_read_state(1,&data));
    assert(SDL_GamepadSensorEnabled(gamepad,SDL_SENSOR_ACCEL));
    assert(SDL_GamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO));
    float accel[3]={SDL_STANDARD_GRAVITY,2*SDL_STANDARD_GRAVITY,-SDL_STANDARD_GRAVITY};
    float gyro[3]={0,0,1.57079632679f};
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_ACCEL,1,accel,3));
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,1,gyro,3));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    assert(!pad_read_state(1,&data));
    assert(fabsf(data.acceleration[0]-1)<1e-6f && fabsf(data.acceleration[1]-2)<1e-6f && fabsf(data.acceleration[2]+1)<1e-6f);
    assert(fabsf(data.angular_velocity[2]-gyro[2])<1e-6f);
    for (int i=1;i<=10;++i) {
        assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_GYRO,1+(Uint64)i*100000000,gyro,3));
        SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    }
    assert(!pad_read_state(1,&data));
    assert(fabsf(data.orientation[2]-sqrtf(0.5f))<1e-5f && fabsf(data.orientation[3]-sqrtf(0.5f))<1e-5f);
    SDL_Event event={.type=SDL_EVENT_GAMEPAD_SENSOR_UPDATE};
    event.gsensor.which=id; event.gsensor.sensor=SDL_SENSOR_GYRO;
    event.gsensor.timestamp=SDL_GetTicksNS(); event.gsensor.sensor_timestamp=500000001;
    event.gsensor.data[0]=100;
    motion_event(NULL,&event);
    assert(!pad_read_state(1,&data) && data.angular_velocity[0]==0);
    assert(!pad_reset_orientation(1)); assert(!pad_read_state(1,&data));
    assert(data.orientation[0]==0 && data.orientation[1]==0 && data.orientation[2]==0 && data.orientation[3]==1);
    event.gsensor.timestamp=SDL_GetTicksNS();
    motion_event(NULL,&event);
    assert(!pad_read_state(1,&data) && data.angular_velocity[0]==0 && data.orientation[3]==1);
    assert(!pad_motion_enable(1,0)); assert(!pad_read_state(1,&data));
    assert(data.acceleration[0]==0 && data.angular_velocity[2]==0);
    assert(!pad_motion_enable(1,1));
    event.gsensor.timestamp=SDL_GetTicksNS();
    motion_event(NULL,&event);
    assert(!pad_read_state(1,&data) && data.angular_velocity[0]==0 && data.orientation[3]==1);
    event.gsensor.sensor_timestamp=1500000001;
    event.gsensor.timestamp=motion.epoch_time-1;
    motion_event(NULL,&event);
    assert(!pad_read_state(1,&data) && data.angular_velocity[0]==0);
    assert(SDL_SendJoystickVirtualSensorData(joystick,SDL_SENSOR_ACCEL,2000000001,accel,3));
    SDL_UpdateJoysticks(); SDL_UpdateGamepads();
    SDL_Delay(260);
    assert(!pad_read_state(1,&data) && data.acceleration[0]==0 && motion.warned);
    SDL_CloseGamepad(gamepad); gamepad=NULL; unsetenv("BB_PAD_MOTION");
    assert(!pad_read_state(1,&data));
    assert(!motion_watching && !motion.enabled && data.orientation[3]==1);
    assert(!SDL_GamepadSensorEnabled(gamepad,SDL_SENSOR_GYRO));
    puts("PASS: SDL motion event samples, g/rad-s units, quaternion rotation, repeated-time rejection, reset/disable/stale data");
#endif
    uint8_t motors[]={17,91};
    assert(!pad_vibration(1,motors));
    assert(rumble_calls && rumble_low==17*257 && rumble_high==91*257);
    fail_rumble=1; motors[0]=18;
    assert(!pad_vibration(1,motors));
#ifdef __APPLE__
    assert(atomic_load(&pad_failures)==1);
#endif
    fail_rumble=0;
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==480);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    fail_init=1; sdl_ready=0;
    assert(!current_gamepad());
#ifdef __APPLE__
    assert(atomic_load(&pad_failures)==2);
#endif
    unlink(path);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture");
}
