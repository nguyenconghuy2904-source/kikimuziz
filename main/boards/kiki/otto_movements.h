#ifndef __OTTO_MOVEMENTS_H__
#define __OTTO_MOVEMENTS_H__

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "oscillator.h"
#include <functional>

//-- Event bits for action control (from PetDog)
#define STOP_ACTION_BIT    BIT0
#define START_ACTION_BIT   BIT1

//-- Constants
#define FORWARD 1
#define BACKWARD -1
#define LEFT 1
#define RIGHT -1
#define BOTH 0
#define SMALL 5
#define MEDIUM 15
#define BIG 30

// -- Servo delta limit default. degree / sec
#define SERVO_LIMIT_DEFAULT 240

// -- Dog-style servo indexes (5 servos - added tail)
#define SERVO_LF 0  // Left Front leg
#define SERVO_RF 1  // Right Front leg  
#define SERVO_LB 2  // Left Back leg
#define SERVO_RB 3  // Right Back leg
#define SERVO_TAIL 4 // Tail servo
#define SERVO_COUNT 5

// Legacy compatibility (deprecated)
#define LEFT_LEG SERVO_LF
#define RIGHT_LEG SERVO_RF
#define LEFT_FOOT SERVO_LB
#define RIGHT_FOOT SERVO_RB

class Otto {
public:
    Otto();
    ~Otto();

    //-- Otto initialization for 5 servos (4-leg + tail)
    void Init(int left_front, int right_front, int left_back, int right_back, int tail = -1);
    
    //-- Attach & detach functions
    void AttachServos();
    void DetachServos();

    //-- Servo Trims for 5 servos
    void SetTrims(int left_front, int right_front, int left_back, int right_back, int tail = 0);

    //-- Basic servo control functions (from DogMaster style)
    void ServoWrite(int servo_id, float angle);
    void ServoAngleSet(int servo_id, float angle, int delay_time);
    void ServoInit(int lf_angle, int rf_angle, int lb_angle, int rb_angle, int delay_time, int tail_angle = 90);
    int GetServoAngle(int servo_id);  // Get current servo angle

    //-- HOME = Otto at rest position
    void Home();
    void StandUp();
    bool GetRestState();
    void SetRestState(bool state);

    //-- Dog-style Movement Functions (adapted from DogMaster)
    void DogWalk(int steps = 2, int speed_delay = 150);
    void DogWalkBack(int steps = 2, int speed_delay = 150);
    void DogTurnLeft(int steps = 3, int speed_delay = 150);
    void DogTurnRight(int steps = 3, int speed_delay = 150);
    void DogSitDown(int delay_time = 500);
    void DogLieDown(int delay_time = 1000);
    void DogJump(int delay_time = 200);
    void DogBow(int delay_time = 2000);
    void DogDance(int cycles = 3, int speed_delay = 200);
    void DogWaveRightFoot(int waves = 5, int speed_delay = 50);
    void DogDance4Feet(int cycles = 6, int speed_delay = 300);
    void DogSwing(int cycles = 8, int speed_delay = 6);
    void DogStretch(int cycles = 2, int speed_delay = 15);
    void DogScratch(int scratches = 5, int speed_delay = 50);  // New: Sit + BR leg scratch (gãi ngứa)
    void WagTail(int wags = 5, int speed_delay = 100);  // New: Wag tail movement
    void DogRollOver(int rolls = 1, int speed_delay = 200);  // New: Roll over movement
    void DogPlayDead(int duration_seconds = 5);  // New: Play dead (lie down and stay still)
    void DogShakePaw(int shakes = 3, int speed_delay = 150);  // New: Shake paw (bắt tay)
    void DogSidestep(int steps = 3, int speed_delay = 150, int direction = 1);  // New: Sidestep (đi ngang)
    void DogPushup(int pushups = 3, int speed_delay = 150);  // New: Pushup exercise
    void DogBalance(int duration_ms = 2000, int speed_delay = 150);  // New: Balance on hind legs
    void DogToilet(int hold_ms = 3000, int speed_delay = 150); // New: Toilet squat pose

    //-- Continuous movement functions (from PetDog - run until Stop() is called)
    void ContinuousWalk(int speed_delay = 150);       // Walk forward continuously
    void ContinuousWalkBack(int speed_delay = 150);   // Walk backward continuously  
    void ContinuousTurnLeft(int speed_delay = 100);   // Turn left continuously
    void ContinuousTurnRight(int speed_delay = 100);  // Turn right continuously
    void Stop();  // Stop any continuous movement
    bool IsActionStopped();  // Check if stop was requested

    //-- Idle system (from PetDog)
    void StartIdleTask();  // Start idle monitoring task
    void SetIdleCallback(std::function<void()> callback);  // Set callback when idle action triggers
    void TriggerRandomIdleAction();  // Perform random idle action

    //-- Legacy movement functions (adapted to work with 4 servos)
    void Jump(float steps = 1, int period = 2000);
    void Walk(float steps = 4, int period = 1000, int dir = FORWARD);
    void Turn(float steps = 4, int period = 2000, int dir = LEFT);
    void Bend(int steps = 1, int period = 1400, int dir = LEFT);

    // -- Servo limiter
    void EnableServoLimit(int speed_limit_degree_per_sec = SERVO_LIMIT_DEFAULT);
    void DisableServoLimit();

private:
    Oscillator servo_[SERVO_COUNT];

    int servo_pins_[SERVO_COUNT];
    int servo_trim_[SERVO_COUNT];
    int servo_compensate_[SERVO_COUNT];  // Compensation angles like DogMaster
    int servo_home_[4];  // Home angles for 4 leg servos: LF, RF, LB, RB

    unsigned long final_time_;
    unsigned long partial_time_;
    float increment_[SERVO_COUNT];

    bool is_otto_resting_;
    int speed_delay_;  // Default speed delay for movements

    // Helper functions for dog movements
    void ExecuteDogMovement(int lf, int rf, int lb, int rb, int delay_time);
    void MoveToPosition(int target_angles[SERVO_COUNT], int move_time);
    
    //-- Event group for action control (from PetDog)
    EventGroupHandle_t action_event_group_;
    TaskHandle_t idle_task_handle_;
    std::function<void()> idle_callback_;
    bool idle_task_running_;
    
    //-- Idle task implementation
    static void IdleTaskWrapper(void* arg);
    void IdleTask();
};

#endif  // __OTTO_MOVEMENTS_H__