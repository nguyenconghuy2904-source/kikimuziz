#include "otto_movements.h"

#include <algorithm>

#include "oscillator.h"
#include "board.h"
#include "display/display.h"

static const char* TAG = "OttoMovements";

Otto::Otto() {
    is_otto_resting_ = false;
    speed_delay_ = 100;  // Reduced to 100ms for faster movement
    
    // Initialize event group for action control (from PetDog)
    action_event_group_ = xEventGroupCreate();
    idle_task_handle_ = nullptr;
    idle_callback_ = nullptr;
    idle_task_running_ = false;
    
    // Initialize all servo pins to -1 (not connected)
    for (int i = 0; i < SERVO_COUNT; i++) {
        servo_pins_[i] = -1;
        servo_trim_[i] = 0;
        servo_compensate_[i] = 0;  // Compensation angles
    }
    
    // Initialize home angles to default 90°
    servo_home_[0] = 90;  // LF
    servo_home_[1] = 90;  // RF
    servo_home_[2] = 90;  // LB
    servo_home_[3] = 90;  // RB
}

Otto::~Otto() {
    // Stop idle task if running
    if (idle_task_handle_ != nullptr) {
        idle_task_running_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));  // Give task time to exit
        // Task will delete itself
    }
    
    // Delete event group
    if (action_event_group_ != nullptr) {
        vEventGroupDelete(action_event_group_);
        action_event_group_ = nullptr;
    }
    
    DetachServos();
}

unsigned long IRAM_ATTR millis() {
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

void Otto::Init(int left_front, int right_front, int left_back, int right_back, int tail) {
    servo_pins_[SERVO_LF] = left_front;
    servo_pins_[SERVO_RF] = right_front;
    servo_pins_[SERVO_LB] = left_back;
    servo_pins_[SERVO_RB] = right_back;
    servo_pins_[SERVO_TAIL] = tail;

    ESP_LOGI(TAG, "Initializing Otto with pins: LF=%d, RF=%d, LB=%d, RB=%d, TAIL=%d", 
             left_front, right_front, left_back, right_back, tail);

    AttachServos();
    is_otto_resting_ = false;
}

///////////////////////////////////////////////////////////////////
//-- ATTACH & DETACH FUNCTIONS ----------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::AttachServos() {
    ESP_LOGI(TAG, "Attaching servos...");
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            ESP_LOGI(TAG, "Attaching servo %d to GPIO %d", i, servo_pins_[i]);
            servo_[i].Attach(servo_pins_[i]);
            ESP_LOGI(TAG, "Servo %d attached successfully", i);
        } else {
            ESP_LOGW(TAG, "Servo %d has invalid pin (-1)", i);
        }
    }
    ESP_LOGI(TAG, "All servos attached");
}

void Otto::DetachServos() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].Detach();
        }
    }
}

///////////////////////////////////////////////////////////////////
//-- SERVO TRIMS & COMPENSATION ---------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::SetTrims(int left_front, int right_front, int left_back, int right_back, int tail) {
    servo_trim_[SERVO_LF] = left_front;
    servo_trim_[SERVO_RF] = right_front;
    servo_trim_[SERVO_LB] = left_back;
    servo_trim_[SERVO_RB] = right_back;
    servo_trim_[SERVO_TAIL] = tail;

    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetTrim(servo_trim_[i]);
        }
    }
}

///////////////////////////////////////////////////////////////////
//-- BASIC DOG-STYLE SERVO CONTROL FUNCTIONS -------------------//
///////////////////////////////////////////////////////////////////
void Otto::ServoWrite(int servo_id, float angle) {
    if (servo_id < 0 || servo_id >= SERVO_COUNT || servo_pins_[servo_id] == -1) {
        return;
    }
    
    // Apply compensation and trim
    angle += servo_compensate_[servo_id] + servo_trim_[servo_id];
    
    // Limit angle to 0-180 degrees
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    
    // For right side servos, invert the angle (like DogMaster)
    if (servo_id == SERVO_RF || servo_id == SERVO_RB) {
        angle = 180 - angle;
    }
    
    servo_[servo_id].SetPosition(angle);
}

void Otto::ServoAngleSet(int servo_id, float angle, int delay_time) {
    ServoWrite(servo_id, angle);
    
    if (delay_time > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_time));
    }
}

int Otto::GetServoAngle(int servo_id) {
    if (servo_id < 0 || servo_id >= SERVO_COUNT || servo_pins_[servo_id] == -1) {
        return 90;  // Default angle
    }
    
    // Get position from oscillator and compensate for right side inversion
    float angle = servo_[servo_id].GetPosition();
    
    // For right side servos, invert back to logical angle
    if (servo_id == SERVO_RF || servo_id == SERVO_RB) {
        angle = 180 - angle;
    }
    
    // Remove compensation and trim to get raw angle
    angle -= (servo_compensate_[servo_id] + servo_trim_[servo_id]);
    
    return (int)angle;
}

void Otto::ServoInit(int lf_angle, int rf_angle, int lb_angle, int rb_angle, int delay_time, int tail_angle) {
    ServoAngleSet(SERVO_LF, lf_angle, 0);
    ServoAngleSet(SERVO_RF, rf_angle, 0);
    ServoAngleSet(SERVO_LB, lb_angle, 0);
    ServoAngleSet(SERVO_RB, rb_angle, 0);
    
    // Initialize tail to specified angle if tail servo is connected
    if (servo_pins_[SERVO_TAIL] != -1) {
        ServoAngleSet(SERVO_TAIL, tail_angle, 0);
    }
    
    if (delay_time > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_time));
    }
    
    ESP_LOGI(TAG, "Dog servo initialized - LF:%d RF:%d LB:%d RB:%d TAIL:%d", 
             lf_angle, rf_angle, lb_angle, rb_angle, tail_angle);
}

void Otto::ExecuteDogMovement(int lf, int rf, int lb, int rb, int delay_time) {
    ServoAngleSet(SERVO_LF, lf, 0);
    ServoAngleSet(SERVO_RF, rf, 0);
    ServoAngleSet(SERVO_LB, lb, 0);
    ServoAngleSet(SERVO_RB, rb, delay_time);
}

void Otto::MoveToPosition(int target_angles[SERVO_COUNT], int move_time) {
    if (GetRestState() == true) {
        SetRestState(false);
    }

    final_time_ = millis() + move_time;
    if (move_time > 10) {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                increment_[i] = (target_angles[i] - servo_[i].GetPosition()) / (move_time / 10.0);
            }
        }

        for (int iteration = 1; millis() < final_time_; iteration++) {
            for (int i = 0; i < SERVO_COUNT; i++) {
                if (servo_pins_[i] != -1) {
                    ServoWrite(i, servo_[i].GetPosition() + increment_[i]);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    } else {
        for (int i = 0; i < SERVO_COUNT; i++) {
            if (servo_pins_[i] != -1) {
                ServoWrite(i, target_angles[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(move_time));
    }

    // Final adjustment to target
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            ServoWrite(i, target_angles[i]);
        }
    }
}

///////////////////////////////////////////////////////////////////
//-- HOME & REST FUNCTIONS --------------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::Home() {
    StandUp();
}

void Otto::StandUp() {
    ESP_LOGI(TAG, "Dog standing up to rest position (smooth relaxed style)");
    
    // Get current positions
    int current_lf = GetServoAngle(SERVO_LF);
    int current_rf = GetServoAngle(SERVO_RF);
    int current_lb = GetServoAngle(SERVO_LB);
    int current_rb = GetServoAngle(SERVO_RB);
    
    // Target standing position: all servos to 90°
    const int target = 90;
    const int step_delay = 12;  // 12ms per step = ~1 second total for smooth movement
    
    // Calculate max distance to move
    int max_steps = 0;
    max_steps = std::max(max_steps, abs(target - current_lf));
    max_steps = std::max(max_steps, abs(target - current_rf));
    max_steps = std::max(max_steps, abs(target - current_lb));
    max_steps = std::max(max_steps, abs(target - current_rb));
    
    // Move all servos smoothly toward 90° simultaneously
    for (int step = 0; step <= max_steps; step++) {
        float progress = (max_steps > 0) ? (float)step / max_steps : 1.0f;
        
        int lf = current_lf + (int)((target - current_lf) * progress);
        int rf = current_rf + (int)((target - current_rf) * progress);
        int lb = current_lb + (int)((target - current_lb) * progress);
        int rb = current_rb + (int)((target - current_rb) * progress);
        
        ServoAngleSet(SERVO_LF, lf, 0);
        ServoAngleSet(SERVO_RF, rf, 0);
        ServoAngleSet(SERVO_LB, lb, 0);
        ServoAngleSet(SERVO_RB, rb, step_delay);
    }
    
    // Final position ensure
    ServoInit(90, 90, 90, 90, 0, 90);
    
    is_otto_resting_ = true;
    vTaskDelay(pdMS_TO_TICKS(300));  // Brief pause after standing
    ESP_LOGI(TAG, "Dog standing up completed smoothly");
}

bool Otto::GetRestState() {
    return is_otto_resting_;
}

void Otto::SetRestState(bool state) {
    is_otto_resting_ = state;
}

///////////////////////////////////////////////////////////////////
//-- DOG-STYLE MOVEMENT FUNCTIONS (from DogMaster) -------------//
///////////////////////////////////////////////////////////////////

//-- Dog Walk Forward (adapted from DogMaster Action_Advance)
void Otto::DogWalk(int steps, int speed_delay) {
    ESP_LOGI(TAG, "Dog walking forward for %d steps", steps);
    
    // Preparation movement to avoid interference
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(120));

    for (int i = 0; i < steps; i++) {
        // Step 1: DogMaster sequence - LF+RB diagonal, then RF+LB
        // Changed from 30°/150° to 35°/145° (reduced by 5° for gentler movement)
        ServoAngleSet(SERVO_LF, 35, 0);
        ServoAngleSet(SERVO_RB, 35, speed_delay);
        ServoAngleSet(SERVO_RF, 145, 0);
        ServoAngleSet(SERVO_LB, 145, speed_delay);

        // Return to neutral with same sequence
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        // Step 2: Opposite diagonal - RF+LB, then LF+RB  
        ServoAngleSet(SERVO_RF, 35, 0);
        ServoAngleSet(SERVO_LB, 35, speed_delay);
        ServoAngleSet(SERVO_LF, 145, 0);
        ServoAngleSet(SERVO_RB, 145, speed_delay);

        // Return to neutral
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
    }
    
    ESP_LOGI(TAG, "Dog walk forward completed");
}

//-- Dog Walk Backward (adapted from DogMaster Action_Back)
void Otto::DogWalkBack(int steps, int speed_delay) {
    ESP_LOGI(TAG, "Dog walking backward for %d steps", steps);
    
    // Preparation movement - same delay as forward
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(120));

    for (int i = 0; i < steps; i++) {
        // Step 1: DogMaster sequence - LF+RB diagonal (reversed angles)
        // Changed from 30°/150° to 35°/145° (reduced by 5° for gentler movement)
        ServoAngleSet(SERVO_LF, 145, 0);    // Reversed: from 35 → 145
        ServoAngleSet(SERVO_RB, 145, speed_delay);
        ServoAngleSet(SERVO_RF, 35, 0);     // Reversed: from 145 → 35
        ServoAngleSet(SERVO_LB, 35, speed_delay);

        // Return to neutral with same sequence
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        // Step 2: Opposite diagonal - RF+LB (reversed angles)
        ServoAngleSet(SERVO_RF, 145, 0);    // Reversed: from 35 → 145
        ServoAngleSet(SERVO_LB, 145, speed_delay);
        ServoAngleSet(SERVO_LF, 35, 0);     // Reversed: from 145 → 35
        ServoAngleSet(SERVO_RB, 35, speed_delay);

        // Return to neutral
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
    }
    
    ESP_LOGI(TAG, "Dog walk backward completed");
}

//-- Dog Turn Left (adapted from DogMaster Action_TurnLeft)
void Otto::DogTurnLeft(int steps, int speed_delay) {
    ESP_LOGI(TAG, "Dog turning left for %d steps", steps);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(500));

    for (int i = 0; i < steps; i++) {
        // DogMaster sequence: RF+LB first, then LF+RB
        ServoAngleSet(SERVO_RF, 45, 0);
        ServoAngleSet(SERVO_LB, 135, speed_delay);
        ServoAngleSet(SERVO_LF, 45, 0);
        ServoAngleSet(SERVO_RB, 135, speed_delay);

        // Return to neutral
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
    }
    
    ESP_LOGI(TAG, "Dog turn left completed");
}

//-- Dog Turn Right (adapted from DogMaster Action_TurnRight)
void Otto::DogTurnRight(int steps, int speed_delay) {
    ESP_LOGI(TAG, "Dog turning right for %d steps", steps);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(500));

    for (int i = 0; i < steps; i++) {
        // DogMaster sequence: LF+RB first, then RF+LB  
        ServoAngleSet(SERVO_LF, 45, 0);
        ServoAngleSet(SERVO_RB, 135, speed_delay);
        ServoAngleSet(SERVO_RF, 45, 0);
        ServoAngleSet(SERVO_LB, 135, speed_delay);

        // Return to neutral
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
    }
    
    ESP_LOGI(TAG, "Dog turn right completed");
}

//-- Dog Sit Down (adapted from DogMaster Action_SitDown)
void Otto::DogSitDown(int delay_time) {
    ESP_LOGI(TAG, "Dog sitting down smoothly");
    
    // Get current positions
    int current_lf = GetServoAngle(SERVO_LF);
    int current_rf = GetServoAngle(SERVO_RF);
    int current_lb = GetServoAngle(SERVO_LB);
    int current_rb = GetServoAngle(SERVO_RB);
    
    // Target sitting position: front legs at 90°, back legs at 30°
    const int target_lf = 90;
    const int target_rf = 90;
    const int target_lb = 30;
    const int target_rb = 30;
    const int step_delay = 12;  // 12ms per step = ~1 second for smooth movement
    
    // Calculate max distance to move
    int max_steps = 0;
    max_steps = std::max(max_steps, abs(target_lf - current_lf));
    max_steps = std::max(max_steps, abs(target_rf - current_rf));
    max_steps = std::max(max_steps, abs(target_lb - current_lb));
    max_steps = std::max(max_steps, abs(target_rb - current_rb));
    
    // Move all servos smoothly toward target simultaneously
    for (int step = 0; step <= max_steps; step++) {
        float progress = (max_steps > 0) ? (float)step / max_steps : 1.0f;
        
        int lf = current_lf + (int)((target_lf - current_lf) * progress);
        int rf = current_rf + (int)((target_rf - current_rf) * progress);
        int lb = current_lb + (int)((target_lb - current_lb) * progress);
        int rb = current_rb + (int)((target_rb - current_rb) * progress);
        
        ServoAngleSet(SERVO_LF, lf, 0);
        ServoAngleSet(SERVO_RF, rf, 0);
        ServoAngleSet(SERVO_LB, lb, 0);
        ServoAngleSet(SERVO_RB, rb, step_delay);
    }
    
    // Final position ensure
    ExecuteDogMovement(90, 90, 30, 30, 0);
    
    vTaskDelay(pdMS_TO_TICKS(delay_time));
    ESP_LOGI(TAG, "Dog sit down completed smoothly");
}

//-- Dog Lie Down (adapted from DogMaster Action_LieDown)
void Otto::DogLieDown(int delay_time) {
    ESP_LOGI(TAG, "Dog lying down completely (smooth relaxed style)");
    
    // Get current positions
    int current_lf = GetServoAngle(SERVO_LF);
    int current_rf = GetServoAngle(SERVO_RF);
    int current_lb = GetServoAngle(SERVO_LB);
    int current_rb = GetServoAngle(SERVO_RB);
    
    // Target lying position: all servos to 5°
    const int target = 5;
    const int step_delay = 12;  // 12ms per step = ~1 second total for smooth movement
    
    // Calculate max distance to move
    int max_steps = 0;
    max_steps = std::max(max_steps, abs(target - current_lf));
    max_steps = std::max(max_steps, abs(target - current_rf));
    max_steps = std::max(max_steps, abs(target - current_lb));
    max_steps = std::max(max_steps, abs(target - current_rb));
    
    // Move all servos smoothly toward 5° simultaneously
    for (int step = 0; step <= max_steps; step++) {
        float progress = (max_steps > 0) ? (float)step / max_steps : 1.0f;
        
        int lf = current_lf + (int)((target - current_lf) * progress);
        int rf = current_rf + (int)((target - current_rf) * progress);
        int lb = current_lb + (int)((target - current_lb) * progress);
        int rb = current_rb + (int)((target - current_rb) * progress);
        
        ServoAngleSet(SERVO_LF, lf, 0);
        ServoAngleSet(SERVO_RF, rf, 0);
        ServoAngleSet(SERVO_LB, lb, 0);
        ServoAngleSet(SERVO_RB, rb, step_delay);
    }
    
    // Final position ensure
    ExecuteDogMovement(5, 5, 5, 5, 0);
    
    vTaskDelay(pdMS_TO_TICKS(500));  // Brief pause after lying down
    ESP_LOGI(TAG, "Dog lying down completed smoothly");
}

//-- Dog Jump (adapted from DogMaster Action_Jump)
void Otto::DogJump(int delay_time) {
    ESP_LOGI(TAG, "Dog jumping");
    
    // Prepare to jump - crouch down
    ExecuteDogMovement(60, 60, 60, 60, delay_time);
    
    // Jump up - extend all legs
    ExecuteDogMovement(120, 120, 120, 120, 100);
    
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Land - return to standing
    StandUp();
    
    ESP_LOGI(TAG, "Dog jump completed");
}

//-- Dog Bow (adapted from DogMaster Action_Bow)
void Otto::DogBow(int delay_time) {
    ESP_LOGI(TAG, "Dog bowing smoothly");
    
    // Get current positions
    int current_lf = GetServoAngle(SERVO_LF);
    int current_rf = GetServoAngle(SERVO_RF);
    int current_lb = GetServoAngle(SERVO_LB);
    int current_rb = GetServoAngle(SERVO_RB);
    
    // Target bow position: front legs at 0°, back legs at 90°
    const int target_lf = 0;
    const int target_rf = 0;
    const int target_lb = 90;
    const int target_rb = 90;
    const int step_delay = 12;  // 12ms per step = ~1 second for smooth movement
    
    // Calculate max distance to move
    int max_steps = 0;
    max_steps = std::max(max_steps, abs(target_lf - current_lf));
    max_steps = std::max(max_steps, abs(target_rf - current_rf));
    max_steps = std::max(max_steps, abs(target_lb - current_lb));
    max_steps = std::max(max_steps, abs(target_rb - current_rb));
    
    // Move all servos smoothly toward target simultaneously
    for (int step = 0; step <= max_steps; step++) {
        float progress = (max_steps > 0) ? (float)step / max_steps : 1.0f;
        
        int lf = current_lf + (int)((target_lf - current_lf) * progress);
        int rf = current_rf + (int)((target_rf - current_rf) * progress);
        int lb = current_lb + (int)((target_lb - current_lb) * progress);
        int rb = current_rb + (int)((target_rb - current_rb) * progress);
        
        ServoAngleSet(SERVO_LF, lf, 0);
        ServoAngleSet(SERVO_RF, rf, 0);
        ServoAngleSet(SERVO_LB, lb, 0);
        ServoAngleSet(SERVO_RB, rb, step_delay);
    }
    
    // Final position ensure
    ExecuteDogMovement(0, 0, 90, 90, 0);
    
    vTaskDelay(pdMS_TO_TICKS(delay_time));  // Hold bow position
    
    // Stand up again smoothly
    StandUp();
    
    ESP_LOGI(TAG, "Dog bow completed smoothly");
}

//-- Dog Dance (adapted from DogMaster Action_Dance)
void Otto::DogDance(int cycles, int speed_delay) {
    ESP_LOGI(TAG, "Dog dancing for %d cycles", cycles);
    
    for (int i = 0; i < cycles; i++) {
        // Step 1: Lean left (left side down, right side up)
        ExecuteDogMovement(60, 120, 60, 120, 200);
        
        // Step 2: Lean right (left side up, right side down)
        ExecuteDogMovement(120, 60, 120, 60, 200);
        
        // Step 3: Small jump - crouch down
        ExecuteDogMovement(75, 75, 105, 105, 150);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // Jump up
        ExecuteDogMovement(105, 105, 75, 75, 150);
    }
    
    // End with standing position
    StandUp();
    
    ESP_LOGI(TAG, "Dog dance completed");
}

//-- Dog Wave Right Foot (adapted from DogMaster Action_WaveRightFoot) - SITTING VERSION
void Otto::DogWaveRightFoot(int waves, int speed_delay) {
    ESP_LOGI(TAG, "Dog waving right front foot %d times (sitting)", waves);
    
    // Prepare sitting position: LF standing (90°), RF ready to wave (90°), back legs sitting (30°)
    ExecuteDogMovement(90, 90, 30, 30, 300);
    
    // Wave right front leg from 90° to 0° (slower: 16ms delay instead of 8ms)
    // LF stays at 90° (standing), LB and RB stay at 30° (sitting)
    for (int wave_count = 0; wave_count < waves; wave_count++) {
        ESP_LOGI(TAG, "Wave %d (sitting)", wave_count + 1);
        
        // Wave down from 90° to 0°
        for (int angle = 90; angle >= 0; angle -= 5) {
            ServoAngleSet(SERVO_RF, angle, 0);
            vTaskDelay(pdMS_TO_TICKS(16));  // Slower: 16ms (was 8ms)
        }
        
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
        
        // Wave up from 0° to 90°
        for (int angle = 0; angle <= 90; angle += 5) {
            ServoAngleSet(SERVO_RF, angle, 0);
            vTaskDelay(pdMS_TO_TICKS(16));  // Slower: 16ms (was 8ms)
        }
        
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
    }
    
    ESP_LOGI(TAG, "Right foot wave completed (sitting)");
    
    // End with sitting position (already sitting, just ensure proper posture)
    DogSitDown(300);
}

//-- Dog Dance 4 Feet (adapted from DogMaster Action_Dance4Feet)
void Otto::DogDance4Feet(int cycles, int speed_delay) {
    ESP_LOGI(TAG, "Dog dancing with 4 feet for %d cycles", cycles);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    for (int cycle = 0; cycle < cycles; cycle++) {
        // PHASE 1: All feet move forward together
        ESP_LOGI(TAG, "All feet forward");
        ExecuteDogMovement(60, 60, 60, 60, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(400));
        
        // PHASE 2: All feet move backward together
        ESP_LOGI(TAG, "All feet backward");
        ExecuteDogMovement(120, 120, 120, 120, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(400));
        
        // PHASE 3: Return to center (90°)
        ExecuteDogMovement(90, 90, 90, 90, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    // End with firm standing position
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "4-feet dance completed");
}

//-- Dog Swing (adapted from DogMaster Action_Swing)
void Otto::DogSwing(int cycles, int speed_delay) {
    ESP_LOGI(TAG, "Dog swinging for %d cycles", cycles);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(500));

    // Initial lean to prepare
    for (int i = 90; i > 30; i--) {
        ExecuteDogMovement(i, i, i, i, 0);
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
    }
    
    // Swing back and forth
    for (int temp = 0; temp < cycles; temp++) {
        for (int i = 30; i < 90; i++) {
            ExecuteDogMovement(i, 110 - i, i, 110 - i, 0);
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
        }
        for (int i = 90; i > 30; i--) {
            ExecuteDogMovement(i, 110 - i, i, 110 - i, 0);
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
        }
    }
    
    DogSitDown(0);
    
    ESP_LOGI(TAG, "Dog swing completed");
}

//-- Dog Stretch (adapted from DogMaster Action_Stretch)
void Otto::DogStretch(int cycles, int speed_delay) {
    ESP_LOGI(TAG, "Dog stretching for %d cycles", cycles);
    
    ExecuteDogMovement(90, 90, 90, 90, 80);

    for (int i = 0; i < cycles; i++) {
        // Stretch front legs down
        for (int j = 90; j > 10; j--) {
            ExecuteDogMovement(j, j, 90, 90, speed_delay);
        }
        for (int j = 10; j < 90; j++) {
            ExecuteDogMovement(j, j, 90, 90, speed_delay);
        }
        
        // Stretch back legs up
        for (int j = 90; j < 170; j++) {
            ExecuteDogMovement(90, 90, j, j, speed_delay);
        }
        for (int j = 170; j > 90; j--) {
            ExecuteDogMovement(90, 90, j, j, speed_delay);
        }
    }
    
    ESP_LOGI(TAG, "Dog stretch completed");
}

//-- Dog Scratch (gãi ngứa): Sit + BR leg wave continuously
void Otto::DogScratch(int scratches, int speed_delay) {
    ESP_LOGI(TAG, "Dog scratching %d times", scratches);
    
    // Sit down first
    DogSitDown(500);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Wave back-right leg continuously while sitting
    for (int scratch_count = 0; scratch_count < scratches; scratch_count++) {
        ESP_LOGI(TAG, "Scratch %d", scratch_count + 1);
        
        // Scratch motion: RB from 30° down to 0° (then back up to 30°)
        for (int angle = 30; angle >= 0; angle -= 10) {
            ServoAngleSet(SERVO_RB, angle, 0);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
        
        for (int angle = 0; angle <= 30; angle += 10) {
            ServoAngleSet(SERVO_RB, angle, 0);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
    }
    
    ESP_LOGI(TAG, "Dog scratch completed");
    
    // Stay sitting (no auto stand-up)
}

//-- Otto wag tail (new movement with SERVO_TAIL)
void Otto::WagTail(int wags, int speed_delay) {
    if (servo_pins_[SERVO_TAIL] == -1) {
        ESP_LOGW(TAG, "Tail servo not connected, skipping wag tail");
        return;
    }
    
    ESP_LOGI(TAG, "🐕 Wagging tail %d times", wags);
    
    // Center position for tail
    const int tail_center = 90;
    const int tail_left = 30;    // Increased swing angle: was 45, now 30 (more left)
    const int tail_right = 150;  // Increased swing angle: was 135, now 150 (more right)
    
    // Reset to center first
    ServoAngleSet(SERVO_TAIL, tail_center, 0);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Wag left and right
    for (int wag_count = 0; wag_count < wags; wag_count++) {
        ESP_LOGI(TAG, "Wag %d", wag_count + 1);
        
        // Wag to right
        ServoAngleSet(SERVO_TAIL, tail_right, 0);
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
        
        // Wag to left
        ServoAngleSet(SERVO_TAIL, tail_left, 0);
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
    }
    
    // Return to center
    ServoAngleSet(SERVO_TAIL, tail_center, 0);
    ESP_LOGI(TAG, "🐕 Tail wag completed");
}

//-- Dog Roll Over (new movement - lăn qua lăn lại)
void Otto::DogRollOver(int rolls, int speed_delay) {
    ESP_LOGI(TAG, "🐕 Rolling over %d times", rolls);
    
    // Start from lying down position
    DogLieDown(800);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    for (int roll_count = 0; roll_count < rolls; roll_count++) {
        ESP_LOGI(TAG, "Roll %d", roll_count + 1);
        
        // Roll to the right side - all servos move together (SYNC)
        // Lift left side legs up, right side legs stay down
        ExecuteDogMovement(150, 30, 150, 30, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(speed_delay * 2));
        
        // Complete the roll - all legs on ground briefly (SYNC)
        ExecuteDogMovement(90, 90, 90, 90, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
        
        // Now roll back to left - all servos move together (SYNC)
        ExecuteDogMovement(30, 150, 30, 150, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(speed_delay * 2));
        
        // Complete the roll back to original position (SYNC)
        ExecuteDogMovement(90, 90, 90, 90, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
    }
    
    // End by standing up
    StandUp();
    ESP_LOGI(TAG, "🐕 Roll over completed");
}

//-- Dog Play Dead (new movement - giả chết)
void Otto::DogPlayDead(int duration_seconds) {
    ESP_LOGI(TAG, "💀 Playing dead for %d seconds", duration_seconds);
    
    // Lie down dramatically
    DogLieDown(1200);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Stay completely still for the specified duration
    // Legs stay at 5° (lying flat), no movement
    int total_delay = duration_seconds * 1000; // Convert to milliseconds
    int check_interval = 1000; // Check every 1 second
    int checks = total_delay / check_interval;
    
    for (int i = 0; i < checks; i++) {
        ESP_LOGI(TAG, "💀 Still playing dead... (%d/%d seconds)", i + 1, duration_seconds);
        vTaskDelay(pdMS_TO_TICKS(check_interval));
    }
    
    // Slowly "come back to life" - gentle stand up
    ESP_LOGI(TAG, "🐕 Coming back to life...");
    StandUp();
    
    ESP_LOGI(TAG, "🐕 Play dead completed");
}

//-- Dog Shake Paw (bắt tay)
void Otto::DogShakePaw(int shakes, int speed_delay) {
    ESP_LOGI(TAG, "🤝 Shaking paw %d times (fast mode)", shakes);
    
    // Start from standing position
    Home();
    vTaskDelay(pdMS_TO_TICKS(50));  // Faster start
    
    for (int i = 0; i < shakes; i++) {
        // Shift weight slightly to left for balance (SYNC - all servos together)
        ExecuteDogMovement(80, 75, 70, 110, speed_delay / 2);
        vTaskDelay(pdMS_TO_TICKS(40));  // Faster delay
        
        // Lift right front paw (RF to high position) - keep other legs stable
        ExecuteDogMovement(80, 0, 70, 110, speed_delay / 4);
        vTaskDelay(pdMS_TO_TICKS(150));  // Faster hold
        
        // Put paw down quickly (SYNC)
        ExecuteDogMovement(80, 90, 70, 110, speed_delay / 4);
        vTaskDelay(pdMS_TO_TICKS(40));  // Faster delay
    }
    
    // Return to standing
    Home();
    ESP_LOGI(TAG, "🤝 Shake paw completed (fast & high)");
}

//-- Dog Wave Left Foot (vẫy chân trái)
//-- Dog Sidestep (đi ngang)
void Otto::DogSidestep(int steps, int speed_delay, int direction) {
    ESP_LOGI(TAG, "⬅️➡️ Sidestepping %d steps, direction=%d", steps, direction);
    
    // direction: 1 = right, -1 = left
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    for (int i = 0; i < steps; i++) {
        if (direction > 0) {
            // Sidestep RIGHT: lift left side, shift right (SYNC)
            ExecuteDogMovement(120, 80, 120, 80, speed_delay);
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
            
            // Plant left, lift right (SYNC)
            ExecuteDogMovement(80, 120, 80, 120, speed_delay);
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
        } else {
            // Sidestep LEFT: lift right side, shift left (SYNC)
            ExecuteDogMovement(80, 120, 80, 120, speed_delay);
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
            
            // Lift left, plant right (SYNC)
            ExecuteDogMovement(120, 80, 120, 80, speed_delay);
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
        }
    }
    
    Home();
    ESP_LOGI(TAG, "⬅️➡️ Sidestep completed");
}

//-- Dog Pushup (chống đẩy)
void Otto::DogPushup(int pushups, int speed_delay) {
    ESP_LOGI(TAG, "💪 Doing %d pushups", pushups);
    
    // Start in lie down position
    DogLieDown(speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    for (int i = 0; i < pushups; i++) {
        // Push up - front legs extend, back legs stay down (SYNC)
        ExecuteDogMovement(35, 35, 95, 95, speed_delay * 2);
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Down - front legs bend back down (SYNC)
        ExecuteDogMovement(100, 100, 95, 95, speed_delay * 2);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Return to standing
    StandUp();
    ESP_LOGI(TAG, "💪 Pushup completed");
}

//-- Dog Toilet (đi vệ sinh / squat pose)
void Otto::DogToilet(int hold_ms, int speed_delay) {
    ESP_LOGI(TAG, "🚽 Starting toilet squat pose, hold %d ms", hold_ms);

    // Move to a sitting position first for stability
    DogSitDown(speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(400));

    // Squat pose: lower hind legs further, front legs slightly forward (SYNC)
    ExecuteDogMovement(100, 100, 130, 130, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));

    // Small tail wag for realism if tail servo exists
    WagTail(2, 120);

    // Hold squat
    vTaskDelay(pdMS_TO_TICKS(hold_ms));

    // Return via sit then home
    DogSitDown(speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    Home();
    ESP_LOGI(TAG, "🚽 Toilet pose complete");
}

//-- Dog Balance (đứng 2 chân sau - stand on hind legs like kiki-robot)
void Otto::DogBalance(int duration_ms, int speed_delay) {
    ESP_LOGI(TAG, "⚖️ Balancing on hind legs for %d ms", duration_ms);
    
    // Show neutral emotion
    auto display = Board::GetInstance().GetDisplay();
    if (display) {
        display->SetEmotion("neutral");
    }
    
    // Prepare: shift weight back gradually (SYNC)
    ExecuteDogMovement(70, 70, 60, 60, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Lift front legs gradually - stage 1 (SYNC)
    ExecuteDogMovement(100, 100, 50, 50, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Lift front legs more - stage 2 (SYNC)
    ExecuteDogMovement(120, 120, 45, 45, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Balance position - front legs high (SYNC)
    ExecuteDogMovement(140, 140, 40, 40, speed_delay * 2);
    
    // Hold balance
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    
    // Slowly return down - stage 1 (SYNC)
    ExecuteDogMovement(110, 110, 50, 50, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Return to standing - stage 2 (SYNC)
    ExecuteDogMovement(90, 90, 75, 75, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Return to home position
    Home();
    ESP_LOGI(TAG, "⚖️ Balance completed");
}

///////////////////////////////////////////////////////////////////
//-- LEGACY MOVEMENT FUNCTIONS (adapted for 4 servos) ----------//
///////////////////////////////////////////////////////////////////

//-- Otto Jump (simplified for 4 servos)
void Otto::Jump(float steps, int period) {
    ESP_LOGI(TAG, "Legacy jump function");
    DogJump(period / 2);
}

//-- Otto Walk (adapted for 4 servos)  
void Otto::Walk(float steps, int period, int dir) {
    ESP_LOGI(TAG, "Legacy walk function");
    int step_count = (int)steps;
    int speed_delay = period / 4;  // Convert period to speed delay
    
    if (dir == FORWARD) {
        DogWalk(step_count, speed_delay);
    } else {
        DogWalkBack(step_count, speed_delay);
    }
}

//-- Otto Turn (adapted for 4 servos)
void Otto::Turn(float steps, int period, int dir) {
    ESP_LOGI(TAG, "Legacy turn function");
    int step_count = (int)steps;
    int speed_delay = period / 4;
    
    if (dir == LEFT) {
        DogTurnLeft(step_count, speed_delay);
    } else {
        DogTurnRight(step_count, speed_delay);
    }
}

//-- Otto Bend (adapted for 4 servos)
void Otto::Bend(int steps, int period, int dir) {
    ESP_LOGI(TAG, "Legacy bend function");
    DogBow(period);
}

///////////////////////////////////////////////////////////////////
//-- SERVO LIMITER FUNCTIONS ------------------------------------//
///////////////////////////////////////////////////////////////////
void Otto::EnableServoLimit(int diff_limit) {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].SetLimiter(diff_limit);
        }
    }
}

void Otto::DisableServoLimit() {
    for (int i = 0; i < SERVO_COUNT; i++) {
        if (servo_pins_[i] != -1) {
            servo_[i].DisableLimiter();
        }
    }
}

///////////////////////////////////////////////////////////////////
//-- CONTINUOUS MOVEMENT FUNCTIONS (from PetDog) ----------------//
///////////////////////////////////////////////////////////////////

//-- Stop any continuous movement
void Otto::Stop() {
    ESP_LOGI(TAG, "🛑 Stop requested - FORCE CANCEL all movements");
    
    // Set stop bit for continuous movements
    xEventGroupSetBits(action_event_group_, STOP_ACTION_BIT);
    
    // Stop and reset all servo oscillators immediately
    servo_[SERVO_LF].Stop();
    servo_[SERVO_LF].Reset();
    servo_[SERVO_RF].Stop();
    servo_[SERVO_RF].Reset();
    servo_[SERVO_LB].Stop();
    servo_[SERVO_LB].Reset();
    servo_[SERVO_RB].Stop();
    servo_[SERVO_RB].Reset();
    servo_[SERVO_TAIL].Stop();
    servo_[SERVO_TAIL].Reset();
    
    // Wait for oscillators to fully stop
    vTaskDelay(pdMS_TO_TICKS(50));
    
    // Force return to standing position (override everything)
    servo_[SERVO_LF].SetPosition(90);
    servo_[SERVO_RF].SetPosition(90);
    servo_[SERVO_LB].SetPosition(90);
    servo_[SERVO_RB].SetPosition(90);
    
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "✅ All movements FORCEFULLY stopped");
}

//-- Check if stop was requested
bool Otto::IsActionStopped() {
    EventBits_t bits = xEventGroupGetBits(action_event_group_);
    return (bits & STOP_ACTION_BIT) != 0;
}

//-- Continuous walk forward (from PetDog walkfront)
void Otto::ContinuousWalk(int speed_delay) {
    ESP_LOGI(TAG, "🚶 Starting continuous walk forward");
    
    // Clear stop bit before starting
    xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(120));
    
    while (true) {
        // Check for stop signal
        if (IsActionStopped()) {
            ESP_LOGI(TAG, "🛑 Continuous walk stopped");
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }
        
        // Step 1: LF+RB diagonal forward
        ServoAngleSet(SERVO_LF, 35, 0);
        ServoAngleSet(SERVO_RB, 35, speed_delay);
        ServoAngleSet(SERVO_RF, 145, 0);
        ServoAngleSet(SERVO_LB, 145, speed_delay);

        // Return to neutral
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        // Check for stop again
        if (IsActionStopped()) {
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }

        // Step 2: RF+LB diagonal forward
        ServoAngleSet(SERVO_RF, 35, 0);
        ServoAngleSet(SERVO_LB, 35, speed_delay);
        ServoAngleSet(SERVO_LF, 145, 0);
        ServoAngleSet(SERVO_RB, 145, speed_delay);

        // Return to neutral
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
    }
    
    StandUp();
    ESP_LOGI(TAG, "🚶 Continuous walk completed");
}

//-- Continuous walk backward (from PetDog walkBack)
void Otto::ContinuousWalkBack(int speed_delay) {
    ESP_LOGI(TAG, "🔙 Starting continuous walk backward");
    
    xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(120));
    
    while (true) {
        if (IsActionStopped()) {
            ESP_LOGI(TAG, "🛑 Continuous walk back stopped");
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }
        
        // Step 1: Reversed angles for backward
        ServoAngleSet(SERVO_LF, 145, 0);
        ServoAngleSet(SERVO_RB, 145, speed_delay);
        ServoAngleSet(SERVO_RF, 35, 0);
        ServoAngleSet(SERVO_LB, 35, speed_delay);

        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        if (IsActionStopped()) {
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }

        // Step 2
        ServoAngleSet(SERVO_RF, 145, 0);
        ServoAngleSet(SERVO_LB, 145, speed_delay);
        ServoAngleSet(SERVO_LF, 35, 0);
        ServoAngleSet(SERVO_RB, 35, speed_delay);

        ServoAngleSet(SERVO_RF, 90, 0);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, 0);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
    }
    
    StandUp();
    ESP_LOGI(TAG, "🔙 Continuous walk back completed");
}

//-- Continuous turn left (from PetDog turnLeft)
void Otto::ContinuousTurnLeft(int speed_delay) {
    ESP_LOGI(TAG, "↩️ Starting continuous turn left");
    
    xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    while (true) {
        if (IsActionStopped()) {
            ESP_LOGI(TAG, "🛑 Continuous turn left stopped");
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }
        
        // Turn left sequence (from PetDog)
        ServoAngleSet(SERVO_RF, 90, speed_delay);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, speed_delay);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        ServoAngleSet(SERVO_RF, 90, speed_delay);
        ServoAngleSet(SERVO_RB, 50, speed_delay);
        ServoAngleSet(SERVO_LF, 130, speed_delay);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        if (IsActionStopped()) {
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }

        ServoAngleSet(SERVO_RF, 130, speed_delay);
        ServoAngleSet(SERVO_RB, 50, speed_delay);
        ServoAngleSet(SERVO_LF, 130, speed_delay);
        ServoAngleSet(SERVO_LB, 50, speed_delay);

        ServoAngleSet(SERVO_RF, 130, speed_delay);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, speed_delay);
        ServoAngleSet(SERVO_LB, 50, speed_delay);
    }
    
    StandUp();
    ESP_LOGI(TAG, "↩️ Continuous turn left completed");
}

//-- Continuous turn right (from PetDog turnRight)
void Otto::ContinuousTurnRight(int speed_delay) {
    ESP_LOGI(TAG, "↪️ Starting continuous turn right");
    
    xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
    
    StandUp();
    vTaskDelay(pdMS_TO_TICKS(200));
    
    while (true) {
        if (IsActionStopped()) {
            ESP_LOGI(TAG, "🛑 Continuous turn right stopped");
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }
        
        // Turn right sequence (from PetDog - reversed from turn left)
        ServoAngleSet(SERVO_RF, 130, speed_delay);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, speed_delay);
        ServoAngleSet(SERVO_LB, 50, speed_delay);

        ServoAngleSet(SERVO_RF, 130, speed_delay);
        ServoAngleSet(SERVO_RB, 50, speed_delay);
        ServoAngleSet(SERVO_LF, 130, speed_delay);
        ServoAngleSet(SERVO_LB, 50, speed_delay);

        if (IsActionStopped()) {
            xEventGroupClearBits(action_event_group_, STOP_ACTION_BIT);
            break;
        }

        ServoAngleSet(SERVO_RF, 90, speed_delay);
        ServoAngleSet(SERVO_RB, 50, speed_delay);
        ServoAngleSet(SERVO_LF, 130, speed_delay);
        ServoAngleSet(SERVO_LB, 90, speed_delay);

        ServoAngleSet(SERVO_RF, 90, speed_delay);
        ServoAngleSet(SERVO_RB, 90, speed_delay);
        ServoAngleSet(SERVO_LF, 90, speed_delay);
        ServoAngleSet(SERVO_LB, 90, speed_delay);
    }
    
    StandUp();
    ESP_LOGI(TAG, "↪️ Continuous turn right completed");
}

///////////////////////////////////////////////////////////////////
//-- IDLE SYSTEM (from PetDog) ----------------------------------//
///////////////////////////////////////////////////////////////////

//-- Set callback for idle actions
void Otto::SetIdleCallback(std::function<void()> callback) {
    idle_callback_ = callback;
}

//-- Trigger random idle action (from PetDog idle_activate)
void Otto::TriggerRandomIdleAction() {
    ESP_LOGI(TAG, "😴 Triggering random idle action");
    
    int rand_action = rand() % 4;
    
    switch (rand_action) {
        case 0:
            DogStretch(1, 15);
            break;
        case 1:
            DogScratch(3, 50);
            break;
        case 2:
            WagTail(3, 100);
            break;
        case 3:
            DogBow(1500);
            break;
    }
    
    vTaskDelay(pdMS_TO_TICKS(2000));
    DogLieDown(1000);
    
    // Call user callback if set
    if (idle_callback_) {
        idle_callback_();
    }
    
    ESP_LOGI(TAG, "😴 Idle action completed");
}

//-- Idle task wrapper (static for FreeRTOS)
void Otto::IdleTaskWrapper(void* arg) {
    Otto* otto = static_cast<Otto*>(arg);
    otto->IdleTask();
    otto->idle_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

//-- Idle task implementation (from PetDog ActionIdleTask)
void Otto::IdleTask() {
    ESP_LOGI(TAG, "😴 Idle task started");
    
    int rand_time = 0;
    
    while (idle_task_running_) {
        // Random time between 60-180 seconds (like PetDog)
        rand_time = (rand() % 121) + 60;
        ESP_LOGI(TAG, "😴 Next idle action in %d seconds", rand_time);
        
        // Count down
        while (rand_time > 0 && idle_task_running_) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // Check if device is still idle (rest state)
            if (!is_otto_resting_) {
                ESP_LOGI(TAG, "😴 Device active, resetting idle timer");
                rand_time = 0;
                break;
            }
            
            rand_time--;
            
            if (rand_time == 0 && is_otto_resting_) {
                TriggerRandomIdleAction();
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "😴 Idle task stopped");
}

//-- Start idle monitoring task
void Otto::StartIdleTask() {
    if (idle_task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Idle task already running");
        return;
    }
    
    ESP_LOGI(TAG, "😴 Starting idle monitoring task");
    idle_task_running_ = true;
    
    xTaskCreate(
        IdleTaskWrapper,
        "otto_idle_task",
        2048,
        this,
        1,  // Low priority
        &idle_task_handle_
    );
}