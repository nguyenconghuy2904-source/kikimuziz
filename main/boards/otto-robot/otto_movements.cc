#include "otto_movements.h"

#include <algorithm>

#include "oscillator.h"
#include "board.h"
#include "display/display.h"

static const char* TAG = "OttoMovements";

Otto::Otto() {
    is_otto_resting_ = false;
    speed_delay_ = 100;  // Reduced to 100ms for faster movement
    
    // Initialize all servo pins to -1 (not connected)
    for (int i = 0; i < SERVO_COUNT; i++) {
        servo_pins_[i] = -1;
        servo_trim_[i] = 0;
        servo_compensate_[i] = 0;  // Compensation angles
    }
}

Otto::~Otto() {
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

void Otto::ServoInit(int lf_angle, int rf_angle, int lb_angle, int rb_angle, int delay_time, int tail_angle) {
    ServoAngleSet(SERVO_LF, lf_angle, 0);
    ServoAngleSet(SERVO_RF, rf_angle, 0);
    ServoAngleSet(SERVO_LB, lb_angle, 0);
    ServoAngleSet(SERVO_RB, rb_angle, 0);
    
    // Initialize tail to 90 degrees if tail servo is connected
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
    ESP_LOGI(TAG, "Dog standing up to rest position");
    // Increase delay from 500ms to 1200ms for smoother, gentler standing up
    // Tail will be set to 90 degrees (default parameter)
    ServoInit(90, 90, 90, 90, 1200, 90);
    is_otto_resting_ = true;
    vTaskDelay(pdMS_TO_TICKS(500));  // Increased wait time after standing
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
    ESP_LOGI(TAG, "Dog sitting down");
    
    // Front legs stay at 90°, back legs go to 30° to sit
    ExecuteDogMovement(90, 90, 30, 30, delay_time);
    
    ESP_LOGI(TAG, "Dog sit down completed");
}

//-- Dog Lie Down (adapted from DogMaster Action_LieDown)
void Otto::DogLieDown(int delay_time) {
    ESP_LOGI(TAG, "Dog lying down completely");
    
    // Gradually lower all legs to lie flat - slow and gentle (increased delay_time)
    // Use longer delay for smoother transition
    int smooth_delay = (delay_time < 1000) ? 1500 : delay_time;
    ExecuteDogMovement(5, 5, 5, 5, smooth_delay);
    
    vTaskDelay(pdMS_TO_TICKS(1000));  // Hold lying position
    
    ESP_LOGI(TAG, "Dog is now lying completely flat");
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
    ESP_LOGI(TAG, "Dog bowing");
    
    // Bow - front legs down, back legs stay up
    ExecuteDogMovement(0, 0, 90, 90, 100);
    
    vTaskDelay(pdMS_TO_TICKS(delay_time));  // Hold bow position
    
    // Stand up again
    StandUp();
    
    ESP_LOGI(TAG, "Dog bow completed");
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
        
        // Roll to the right side
        // Lift left side legs up, right side legs stay down
        ServoAngleSet(SERVO_LF, 150, 0);  // Left front up
        ServoAngleSet(SERVO_LB, 150, 0);  // Left back up
        ServoAngleSet(SERVO_RF, 30, 0);   // Right front down
        ServoAngleSet(SERVO_RB, 30, speed_delay); // Right back down
        
        vTaskDelay(pdMS_TO_TICKS(speed_delay * 2));
        
        // Complete the roll - all legs on ground briefly
        ExecuteDogMovement(90, 90, 90, 90, speed_delay);
        vTaskDelay(pdMS_TO_TICKS(speed_delay));
        
        // Now on the other side - roll back to left
        ServoAngleSet(SERVO_RF, 150, 0);  // Right front up
        ServoAngleSet(SERVO_RB, 150, 0);  // Right back up
        ServoAngleSet(SERVO_LF, 30, 0);   // Left front down
        ServoAngleSet(SERVO_LB, 30, speed_delay); // Left back down
        
        vTaskDelay(pdMS_TO_TICKS(speed_delay * 2));
        
        // Complete the roll back to original position
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
        // Shift weight slightly to left for balance (faster servo movement)
        // RF uses inverted angles (180 - angle) so: 180-105=75
        ServoAngleSet(SERVO_LF, 80, 0);
        ServoAngleSet(SERVO_RF, 75, 0);  // 180-105 = 75
        ServoAngleSet(SERVO_LB, 70, 0);
        ServoAngleSet(SERVO_RB, 110, speed_delay / 2);  // Faster weight shift
        vTaskDelay(pdMS_TO_TICKS(40));  // Faster delay
        
        // Lift right front paw (RF to high position) - CHANGED: 0° for 180° actual angle
        // 180-180 = 0 (RF inverted) - lift higher!
        ServoAngleSet(SERVO_RF, 0, speed_delay / 4);  // Even faster servo speed
        vTaskDelay(pdMS_TO_TICKS(150));  // Faster hold
        
        // Put paw down quickly
        ServoAngleSet(SERVO_RF, 90, speed_delay / 4);  // Faster down movement
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
            // Sidestep RIGHT: lift left side, shift right
            ServoAngleSet(SERVO_LF, 120, 0);  // Lift left front
            ServoAngleSet(SERVO_RF, 80, 0);   // Plant right front
            ServoAngleSet(SERVO_LB, 120, 0);  // Lift left back
            ServoAngleSet(SERVO_RB, 80, speed_delay);  // Plant right back
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
            
            // Plant left, lift right
            ServoAngleSet(SERVO_LF, 80, 0);   // Plant left front
            ServoAngleSet(SERVO_RF, 120, 0);  // Lift right front
            ServoAngleSet(SERVO_LB, 80, 0);   // Plant left back
            ServoAngleSet(SERVO_RB, 120, speed_delay);  // Lift right back
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
        } else {
            // Sidestep LEFT: lift right side, shift left
            ServoAngleSet(SERVO_LF, 80, 0);   // Plant left front
            ServoAngleSet(SERVO_RF, 120, 0);  // Lift right front
            ServoAngleSet(SERVO_LB, 80, 0);   // Plant left back
            ServoAngleSet(SERVO_RB, 120, speed_delay);  // Lift right back
            vTaskDelay(pdMS_TO_TICKS(speed_delay));
            
            // Lift left, plant right
            ServoAngleSet(SERVO_LF, 120, 0);  // Lift left front
            ServoAngleSet(SERVO_RF, 80, 0);   // Plant right front
            ServoAngleSet(SERVO_LB, 120, 0);  // Lift left back
            ServoAngleSet(SERVO_RB, 80, speed_delay);  // Plant right back
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
        // Push up - front legs extend, back legs stay down
        ServoAngleSet(SERVO_LF, 35, 0);   // Front legs high
        ServoAngleSet(SERVO_RF, 35, 0);
        ServoAngleSet(SERVO_LB, 95, 0);   // Back legs low (neutral-ish)
        ServoAngleSet(SERVO_RB, 95, speed_delay * 2);
        vTaskDelay(pdMS_TO_TICKS(500));
        
        // Down - front legs bend back down
        ServoAngleSet(SERVO_LF, 100, 0);   // Front legs down
        ServoAngleSet(SERVO_RF, 100, 0);
        ServoAngleSet(SERVO_LB, 95, 0);   // Back legs stay
        ServoAngleSet(SERVO_RB, 95, speed_delay * 2);
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

    // Squat pose: lower hind legs further, front legs slightly forward
    // Angles chosen empirically relative to pushup/balance positions
    ServoAngleSet(SERVO_LF, 100, 0);  // Front moderate
    ServoAngleSet(SERVO_RF, 100, 0);
    ServoAngleSet(SERVO_LB, 130, 0);  // Hind deeper
    ServoAngleSet(SERVO_RB, 130, speed_delay * 2);
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
    
    // Prepare: shift weight back gradually (same as kiki-robot)
    ServoAngleSet(SERVO_LF, 70, 0);
    ServoAngleSet(SERVO_RF, 70, 0);
    ServoAngleSet(SERVO_LB, 60, 0);
    ServoAngleSet(SERVO_RB, 60, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(500));
    
    // Lift front legs gradually - stage 1
    ServoAngleSet(SERVO_LF, 100, 0);
    ServoAngleSet(SERVO_RF, 100, 0);
    ServoAngleSet(SERVO_LB, 50, 0);
    ServoAngleSet(SERVO_RB, 50, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Lift front legs more - stage 2
    ServoAngleSet(SERVO_LF, 120, 0);
    ServoAngleSet(SERVO_RF, 120, 0);
    ServoAngleSet(SERVO_LB, 45, 0);
    ServoAngleSet(SERVO_RB, 45, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Balance position - front legs high (LF/RF=140°, LB/RB=40° from kiki-robot)
    ServoAngleSet(SERVO_LF, 140, 0);
    ServoAngleSet(SERVO_RF, 140, 0);
    ServoAngleSet(SERVO_LB, 40, 0);
    ServoAngleSet(SERVO_RB, 40, speed_delay * 2);
    
    // Hold balance
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    
    // Slowly return down - stage 1
    ServoAngleSet(SERVO_LF, 110, 0);
    ServoAngleSet(SERVO_RF, 110, 0);
    ServoAngleSet(SERVO_LB, 50, 0);
    ServoAngleSet(SERVO_RB, 50, speed_delay * 2);
    vTaskDelay(pdMS_TO_TICKS(300));
    
    // Return to standing - stage 2
    ServoAngleSet(SERVO_LF, 90, 0);
    ServoAngleSet(SERVO_RF, 90, 0);
    ServoAngleSet(SERVO_LB, 75, 0);
    ServoAngleSet(SERVO_RB, 75, speed_delay * 2);
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