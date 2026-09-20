// structgen fixture: two fields whose generated accessor names collide.
#pragma once
typedef struct { char text[8]; int text_str; } Collide;
