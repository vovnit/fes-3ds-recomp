// On-screen controls: D-pad, left stick, the four face buttons, L1/L2/R1/R2, SELECT, START and a menu button.
// They are drawn as see-through outlines; `opacity` sets how visible they are at rest (pressed parts light up).
#pragma once

#import <UIKit/UIKit.h>

@interface TouchControls : UIView
@property(nonatomic) CGFloat opacity;              // 0.05 to 1
@property(nonatomic, copy) void (^onMenu)(void);   // the menu button was tapped
@property(nonatomic) BOOL menuOnly;                // only the menu button (a game controller is connected)
- (void)releaseAll;                                // lets go of everything, e.g. when the controls are hidden
@end
