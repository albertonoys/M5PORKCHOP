// Piglet ASCII avatar implementation

#include "avatar.h"
#include "../ui/display.h"

// Static members
AvatarState Avatar::currentState = AvatarState::NEUTRAL;
bool Avatar::isBlinking = false;
bool Avatar::earsUp = true;
uint32_t Avatar::lastBlinkTime = 0;
uint32_t Avatar::blinkInterval = 3000;

// Internal state for looking direction
static bool facingRight = false;
static uint32_t lastFlipTime = 0;
static uint32_t flipInterval = 5000;

// --- DERPY STYLE with direction ---
// Left facing frames (eye on left, snout 00 on right)
const char* AVATAR_NEUTRAL_L[] = {
    " ?  ? ",
    "(o 00)",
    "(    )"
};

const char* AVATAR_HAPPY_L[] = {
    " ^  ^ ",
    "(^ 00)",
    "(    )"
};

const char* AVATAR_EXCITED_L[] = {
    " !  ! ",
    "(@ 00)",
    "(    )"
};

const char* AVATAR_HUNTING_L[] = {
    " /  \\ ",
    "(> 00)",
    "(    )"
};

const char* AVATAR_SLEEPY_L[] = {
    " v  v ",
    "(- 00)",
    "(    )"
};

const char* AVATAR_SAD_L[] = {
    " .  . ",
    "(T 00)",
    "(    )"
};

const char* AVATAR_ANGRY_L[] = {
    " \\  / ",
    "(# 00)",
    "(    )"
};

const char* AVATAR_BLINK_L[] = {
    " ?  ? ",
    "(- 00)",
    "(    )"
};

// Right facing frames (snout 00 on left, eye on right)
const char* AVATAR_NEUTRAL_R[] = {
    " ?  ? ",
    "(00 o)",
    "(    )"
};

const char* AVATAR_HAPPY_R[] = {
    " ^  ^ ",
    "(00 ^)",
    "(    )"
};

const char* AVATAR_EXCITED_R[] = {
    " !  ! ",
    "(00 @)",
    "(    )"
};

const char* AVATAR_HUNTING_R[] = {
    " /  \\ ",
    "(00 <)",
    "(    )"
};

const char* AVATAR_SLEEPY_R[] = {
    " v  v ",
    "(00 -)",
    "(    )"
};

const char* AVATAR_SAD_R[] = {
    " .  . ",
    "(00 T)",
    "(    )"
};

const char* AVATAR_ANGRY_R[] = {
    " \\  / ",
    "(00 #)",
    "(    )"
};

const char* AVATAR_BLINK_R[] = {
    " ?  ? ",
    "(00 -)",
    "(    )"
};

void Avatar::init() {
    currentState = AvatarState::NEUTRAL;
    isBlinking = false;
    earsUp = true;
    lastBlinkTime = millis();
    blinkInterval = random(4000, 8000);
    
    // Init direction
    facingRight = false;
    lastFlipTime = millis();
    flipInterval = random(3000, 10000);
}

void Avatar::setState(AvatarState state) {
    currentState = state;
}

void Avatar::blink() {
    isBlinking = true;
}

void Avatar::wiggleEars() {
    earsUp = !earsUp;
}

void Avatar::draw(M5Canvas& canvas) {
    uint32_t now = millis();

    // Check if we should blink
    if (now - lastBlinkTime > blinkInterval) {
        isBlinking = true;
        lastBlinkTime = now;
        blinkInterval = random(4000, 8000);
    }

    // Check if we should flip direction (look around)
    if (now - lastFlipTime > flipInterval) {
        facingRight = !facingRight;
        lastFlipTime = now;
        flipInterval = random(5000, 15000);
    }
    
    // Select frame based on state and direction
    const char** frame;
    
    if (isBlinking && currentState != AvatarState::SLEEPY) {
        frame = facingRight ? AVATAR_BLINK_R : AVATAR_BLINK_L;
        isBlinking = false;
    } else {
        switch (currentState) {
            case AvatarState::HAPPY:    
                frame = facingRight ? AVATAR_HAPPY_R : AVATAR_HAPPY_L; break;
            case AvatarState::EXCITED:  
                frame = facingRight ? AVATAR_EXCITED_R : AVATAR_EXCITED_L; break;
            case AvatarState::HUNTING:  
                frame = facingRight ? AVATAR_HUNTING_R : AVATAR_HUNTING_L; break;
            case AvatarState::SLEEPY:   
                frame = facingRight ? AVATAR_SLEEPY_R : AVATAR_SLEEPY_L; break;
            case AvatarState::SAD:      
                frame = facingRight ? AVATAR_SAD_R : AVATAR_SAD_L; break;
            case AvatarState::ANGRY:    
                frame = facingRight ? AVATAR_ANGRY_R : AVATAR_ANGRY_L; break;
            default:                    
                frame = facingRight ? AVATAR_NEUTRAL_R : AVATAR_NEUTRAL_L; break;
        }
    }
    
    drawFrame(canvas, frame, 3);
}

void Avatar::drawFrame(M5Canvas& canvas, const char** frame, uint8_t lines) {
    canvas.setTextDatum(top_left);
    canvas.setTextSize(3);
    canvas.setTextColor(COLOR_ACCENT);
    
    int startX = 2;
    int startY = 5;
    int lineHeight = 22;
    
    for (uint8_t i = 0; i < lines; i++) {
        canvas.drawString(frame[i], startX, startY + i * lineHeight);
    }
}
