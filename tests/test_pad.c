#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
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
static float mouse_dx, mouse_dy;
static uint32_t mouse_buttons;
int bbgpu_mouse_take(float *dx, float *dy, uint32_t *buttons) {
    *dx=mouse_dx; *dy=mouse_dy; *buttons=mouse_buttons;
    mouse_dx=mouse_dy=0;
    return 1;
}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static PadData neutral(void) {
    PadData d;
    memset(&d,0,sizeof(d));
    d.left_x=d.left_y=d.right_x=d.right_y=128;
    return d;
}

/* Elden Ring PC defaults translated to the PS4 pad (docs/macos/keyboard-mouse.md). */
static void keyboard_mouse_mapping(void) {
    static bool k[SDL_SCANCODE_COUNT];
    uint64_t t=1000000;
    mouse_cam.sensitivity=0;
    setenv("BB_MOUSE_SENSITIVITY","1",1);
    PadData d=neutral();
    keyboard_mouse(&d,k,0,0,0,t);
    assert(d.buttons==0 && d.left_x==128 && d.left_y==128 && d.right_x==128 && d.right_y==128 && !d.l2 && !d.r2);
    static const struct { SDL_Scancode key; uint32_t ps; } keys[]={
        {SDL_SCANCODE_SPACE,BTN_CIRCLE}, {SDL_SCANCODE_E,BTN_CROSS}, {SDL_SCANCODE_RETURN,BTN_CROSS},
        {SDL_SCANCODE_R,BTN_TRIANGLE}, {SDL_SCANCODE_F,BTN_SQUARE}, {SDL_SCANCODE_Q,BTN_R3},
        {SDL_SCANCODE_X,BTN_L3}, {SDL_SCANCODE_ESCAPE,BTN_OPTIONS}, {SDL_SCANCODE_UP,BTN_UP},
        {SDL_SCANCODE_DOWN,BTN_DOWN}, {SDL_SCANCODE_LEFT,BTN_LEFT}, {SDL_SCANCODE_RIGHT,BTN_RIGHT},
    };
    for (size_t i=0;i<sizeof(keys)/sizeof(*keys);++i) {
        k[keys[i].key]=true; d=neutral();
        keyboard_mouse(&d,k,0,0,0,t);
        assert(d.buttons==keys[i].ps && d.left_x==128 && d.right_x==128);
        k[keys[i].key]=false;
    }
    k[SDL_SCANCODE_TAB]=true; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.buttons==BTN_TOUCHPAD && d.touch_count==1 && d.touches[0].x==480);
    k[SDL_SCANCODE_TAB]=false; k[SDL_SCANCODE_BACKSPACE]=true; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.buttons==BTN_TOUCHPAD && d.touches[0].x==1440);
    k[SDL_SCANCODE_BACKSPACE]=false;
    /* Clicks: Shift at the press is reported by the window as a separate bit. */
    static const struct { uint32_t bit, ps; } clicks[]={
        {BBGPU_MOUSE_LEFT,BTN_R1}, {BBGPU_MOUSE_LEFT_SHIFT,BTN_R2}, {BBGPU_MOUSE_RIGHT,BTN_L2},
        {BBGPU_MOUSE_RIGHT_SHIFT,BTN_L1}, {BBGPU_MOUSE_MIDDLE,BTN_R3},
    };
    for (size_t i=0;i<sizeof(clicks)/sizeof(*clicks);++i) {
        d=neutral(); keyboard_mouse(&d,k,0,0,clicks[i].bit,t);
        assert(d.buttons==clicks[i].ps);
        assert(d.l2==(clicks[i].ps==BTN_L2 ? 255 : 0) && d.r2==(clicks[i].ps==BTN_R2 ? 255 : 0));
    }
    /* WASD full deflection; Alt walks at 40%; IJKL turns the camera. */
    k[SDL_SCANCODE_W]=k[SDL_SCANCODE_D]=true; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.left_x==255 && d.left_y==0 && d.right_x==128);
    k[SDL_SCANCODE_LALT]=true; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.left_x==128+50 && d.left_y==128-51);
    k[SDL_SCANCODE_W]=k[SDL_SCANCODE_D]=k[SDL_SCANCODE_LALT]=false;
    k[SDL_SCANCODE_J]=k[SDL_SCANCODE_K]=true; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.right_x==0 && d.right_y==255 && d.left_x==128);
    k[SDL_SCANCODE_J]=k[SDL_SCANCODE_K]=false;
    /* With a gamepad: idle keys keep its sticks and buttons; a pressed key adds or replaces. */
    d=neutral(); d.left_x=40; d.right_y=200; d.buttons=BTN_CROSS;
    keyboard_mouse(&d,k,0,0,0,t);
    assert(d.left_x==40 && d.right_y==200 && d.buttons==BTN_CROSS);
    k[SDL_SCANCODE_A]=true; keyboard_mouse(&d,k,0,0,BBGPU_MOUSE_LEFT,t);
    assert(d.left_x==0 && d.left_y==128 && d.right_y==200 && d.buttons==(BTN_CROSS|BTN_R1));
    k[SDL_SCANCODE_A]=false;
    /* Mouse camera: 800 points/s is full deflection; slow motion still clears the dead zone;
     * reads closer than 8 ms keep the last deflection; a still mouse is neutral. */
    t+=10000; d=neutral(); keyboard_mouse(&d,k,8,0,0,t);
    assert(d.right_x==255 && d.right_y==128);
    t+=2000; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.right_x==255);
    t+=8000; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.right_x==128 && d.right_y==128);
    t+=10000; d=neutral(); keyboard_mouse(&d,k,1,0,0,t); /* 100 points/s */
    assert(d.right_x==128+33 && d.right_y==128);
    t+=10000; d=neutral(); keyboard_mouse(&d,k,0,-80,0,t); /* past full: clamped */
    assert(d.right_x==128 && d.right_y==1);
    t+=10000; d=neutral(); keyboard_mouse(&d,k,6,8,0,t); /* 1000 points/s diagonally: radial */
    assert(d.right_x==128+76 && d.right_y==128+102);
    /* A pause longer than 100 ms is not read as slow motion. */
    t+=1000000; d=neutral(); keyboard_mouse(&d,k,80,0,0,t);
    assert(d.right_x==255);
    /* BB_MOUSE_SENSITIVITY=2 doubles the turn speed; a malformed value stops. */
    mouse_cam.sensitivity=0; setenv("BB_MOUSE_SENSITIVITY","2",1);
    t+=10000; keyboard_mouse(&d,k,0,0,0,t);
    t+=10000; d=neutral(); keyboard_mouse(&d,k,4,0,0,t);
    assert(d.right_x==255);
    const char *bad[]={"0","-1","fast","nan","101"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        fflush(stdout);
        pid_t child=fork();
        assert(child>=0);
        if (!child) {
            if (!freopen("/dev/null","w",stderr)) _exit(2);
            mouse_cam.sensitivity=0; setenv("BB_MOUSE_SENSITIVITY",bad[i],1);
            keyboard_mouse(&d,k,0,0,0,t);
            _exit(0);
        }
        int status=0;
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==1);
    }
    unsetenv("BB_MOUSE_SENSITIVITY"); mouse_cam.sensitivity=0;
    keyboard_mouse(&d,k,0,0,0,t);
    t+=10000; d=neutral(); keyboard_mouse(&d,k,0,0,0,t);
    assert(d.buttons==0 && d.right_x==128 && d.left_x==128);
    puts("PASS: Elden Ring keyboard/mouse mapping, Shift clicks, walk, gamepad merge, mouse camera, sensitivity");
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
    keyboard_mouse_mapping();
    /* Route input and the mouse add up; a released mouse leaves nothing held; the settings menu
     * hides clicks still held when it closes until they are released. */
    inject(path,"cross");
    mouse_buttons=BBGPU_MOUSE_LEFT;
    assert(pad_read_state(1,&data)==0 && data.buttons==(BTN_CROSS|BTN_R1));
    mouse_buttons=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_CROSS);
    inject(path,"");
    mouse_buttons=BBGPU_MOUSE_RIGHT;
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    capture=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && !data.l2);
    mouse_buttons=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && !data.l2);
    mouse_buttons=BBGPU_MOUSE_RIGHT;
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_L2);
    mouse_buttons=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0);

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
    puts("PASS: pad ABI, keyboard/mouse with route input, debug camera chord, left/right clicks, SDL touch coordinates, overlay capture");
}
