// On-screen controls for the iOS front end (see touch.h). Each finger is tied to the control it first touched:
// the stick follows it, the D-pad and the face buttons follow it as it slides (two face buttons at once when it sits
// between them), and the other buttons stay pressed while it is on them.
#import "touch.h"

#include "frontend.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace fe {
std::atomic<std::uint16_t> g_touch_buttons{0};
std::atomic<std::uint16_t> g_touch_stick{0x8080};
}

using namespace fe;

namespace {

enum class Kind { none, stick, dpad, face, key };

struct Key {
    std::uint16_t mask;   // 0: the menu button
    NSString *title;
    CGRect rect;
    CAShapeLayer *shape;
    CATextLayer *text;
};

struct Track {
    Kind kind = Kind::none;
    std::size_t key = 0;
    std::uint16_t mask = 0;
};

// the face buttons, in the order of their positions: top, right, bottom, left
constexpr std::uint16_t kFaceMask[4] = {kTriangle, kCircle, kCross, kSquare};
constexpr std::uint16_t kDpadMask[4] = {kUp, kRight, kDown, kLeft};
const CGPoint kDirection[4] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

CGFloat distance(CGPoint a, CGPoint b) { return std::hypot(a.x - b.x, a.y - b.y); }

UIColor *face_color(int i) {
    switch (i) {
    case 0: return [UIColor colorWithRed:0.35 green:0.90 blue:0.80 alpha:1];   // triangle
    case 1: return [UIColor colorWithRed:1.00 green:0.45 blue:0.45 alpha:1];   // circle
    case 2: return [UIColor colorWithRed:0.55 green:0.70 blue:1.00 alpha:1];   // cross
    default: return [UIColor colorWithRed:0.96 green:0.62 blue:0.80 alpha:1];  // square
    }
}

}

@implementation TouchControls {
    CGFloat _u;   // size unit: 1 on a phone held sideways
    CGPoint _stickCenter, _dpadCenter, _faceCenter;
    CGFloat _stickRadius, _knobRadius, _dpadRadius, _faceOffset, _faceRadius;
    CAShapeLayer *_stickBase, *_stickKnob, *_dpad[4], *_face[4], *_faceMark[4];
    std::vector<Key> _keys;
    std::map<const void *, Track> _tracks;
    const void *_stickTouch;
    CGPoint _stick;   // -1..1 each way
    std::uint16_t _lit, _held;
    CFTimeInterval _pressedAt[16];
}

// A quick tap can start and end between two reads of the pad, and the game would never see it: every press lasts at least this long.
constexpr CFTimeInterval kMinimumPress = 0.1;

- (instancetype)initWithFrame:(CGRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    self.multipleTouchEnabled = YES;
    self.backgroundColor = UIColor.clearColor;
    _opacity = 0.3;
    _stick = CGPointZero;
    _stickTouch = nullptr;
    _stickBase = [self newShape];
    _stickKnob = [self newShape];
    for (int i = 0; i < 4; ++i) {
        _dpad[i] = [self newShape];
        _face[i] = [self newShape];
        _faceMark[i] = [CAShapeLayer layer];
        _faceMark[i].fillColor = UIColor.clearColor.CGColor;
        _faceMark[i].strokeColor = face_color(i).CGColor;
        [_face[i] addSublayer:_faceMark[i]];
    }
    const struct { std::uint16_t mask; NSString *title; } keys[] = {
        {kL2, @"L2"}, {kL1, @"L1"}, {kR1, @"R1"}, {kR2, @"R2"}, {kSelect, @"SELECT"}, {kStart, @"START"}, {0, @"MENU"},
    };
    for (const auto &k : keys) {
        Key key{k.mask, k.title, CGRectZero, [self newShape], [CATextLayer layer]};
        key.text.string = k.title;
        key.text.alignmentMode = kCAAlignmentCenter;
        key.text.foregroundColor = UIColor.whiteColor.CGColor;
        key.text.contentsScale = UIScreen.mainScreen.scale;
        [key.shape addSublayer:key.text];
        _keys.push_back(key);
    }
    [self restyle];
    return self;
}

- (CAShapeLayer *)newShape {
    CAShapeLayer *s = [CAShapeLayer layer];
    s.strokeColor = [UIColor colorWithWhite:1 alpha:0.85].CGColor;
    s.fillColor = [UIColor colorWithWhite:1 alpha:0.10].CGColor;
    s.actions = @{@"opacity" : [NSNull null], @"position" : [NSNull null], @"bounds" : [NSNull null], @"path" : [NSNull null],
                  @"fillColor" : [NSNull null]};
    [self.layer addSublayer:s];
    return s;
}

- (void)setMenuOnly:(BOOL)menuOnly {
    _menuOnly = menuOnly;
    [self releaseAll];
    _stickBase.hidden = _stickKnob.hidden = menuOnly;
    for (int i = 0; i < 4; ++i) _dpad[i].hidden = _face[i].hidden = menuOnly;
    for (Key &k : _keys) k.shape.hidden = menuOnly && k.mask != 0;
}

- (void)setOpacity:(CGFloat)opacity {
    _opacity = std::clamp<CGFloat>(opacity, 0.05, 1.0);
    [self restyle];
}

// at rest every part has the chosen opacity; a pressed part is drawn filled and more visible
- (void)style:(CAShapeLayer *)s pressed:(bool)pressed {
    s.opacity = static_cast<float>(pressed ? std::min<CGFloat>(1.0, _opacity * 1.8 + 0.25) : _opacity);
    s.fillColor = [UIColor colorWithWhite:1 alpha:pressed ? 0.45 : 0.10].CGColor;
}

- (void)restyle {
    const std::uint16_t lit = _lit;
    for (int i = 0; i < 4; ++i) {
        [self style:_dpad[i] pressed:(lit & kDpadMask[i]) != 0];
        [self style:_face[i] pressed:(lit & kFaceMask[i]) != 0];
    }
    for (Key &k : _keys) [self style:k.shape pressed:k.mask ? (lit & k.mask) != 0 : [self menuHeld]];
    [self style:_stickBase pressed:false];
    [self style:_stickKnob pressed:_stickTouch != nullptr];
}

- (bool)menuHeld {
    for (const auto &[touch, t] : _tracks)
        if (t.kind == Kind::key && _keys[t.key].mask == 0) return true;
    return false;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGRect safe = UIEdgeInsetsInsetRect(self.bounds, self.safeAreaInsets);
    const CGFloat u = _u = std::clamp<CGFloat>(safe.size.height / 360.0, 0.85, 1.6);
    const CGFloat minX = CGRectGetMinX(safe) + 4, maxX = CGRectGetMaxX(safe) - 4, minY = CGRectGetMinY(safe), maxY = CGRectGetMaxY(safe);

    _dpadCenter = CGPointMake(minX + 74 * u, minY + 128 * u);
    _dpadRadius = 58 * u;
    _stickCenter = CGPointMake(minX + 112 * u, maxY - 78 * u);
    _stickRadius = 50 * u;
    _knobRadius = 24 * u;
    _faceCenter = CGPointMake(maxX - 74 * u, minY + 128 * u);
    _faceOffset = 39 * u;
    _faceRadius = 21 * u;

    const CGFloat kw = 58 * u, kh = 30 * u;
    for (Key &k : _keys) {
        switch (k.mask) {
        case kL2: k.rect = CGRectMake(minX + 8 * u, minY + 8 * u, kw, kh); break;
        case kL1: k.rect = CGRectMake(minX + 16 * u + kw, minY + 8 * u, kw, kh); break;
        case kR1: k.rect = CGRectMake(maxX - 16 * u - 2 * kw, minY + 8 * u, kw, kh); break;
        case kR2: k.rect = CGRectMake(maxX - 8 * u - kw, minY + 8 * u, kw, kh); break;
        case kSelect: k.rect = CGRectMake(maxX - 150 * u, maxY - 52 * u, 64 * u, 26 * u); break;
        case kStart: k.rect = CGRectMake(maxX - 78 * u, maxY - 52 * u, 64 * u, 26 * u); break;
        default: k.rect = CGRectMake(minX + 8 * u, maxY - 32 * u, 48 * u, 24 * u); break;   // menu
        }
        k.shape.frame = k.rect;
        k.shape.lineWidth = 1.5;
        k.shape.path = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, k.rect.size.width, k.rect.size.height) cornerRadius:k.rect.size.height / 2].CGPath;
        const CGFloat font = (k.mask == kSelect || k.mask == kStart || k.mask == 0 ? 10 : 13) * u;
        k.text.font = (__bridge CFTypeRef)[UIFont systemFontOfSize:font weight:UIFontWeightSemibold];   // a UIFont is a CTFont
        k.text.fontSize = font;
        k.text.frame = CGRectMake(0, (k.rect.size.height - font * 1.25) / 2, k.rect.size.width, font * 1.3);
    }

    // D-pad: four arms around an empty middle, each with an arrow
    const CGFloat arm = 38 * u, inner = 17 * u;
    for (int i = 0; i < 4; ++i) {
        CGRect r;
        switch (i) {
        case 0: r = CGRectMake(_dpadCenter.x - arm / 2, _dpadCenter.y - _dpadRadius, arm, _dpadRadius - inner); break;
        case 1: r = CGRectMake(_dpadCenter.x + inner, _dpadCenter.y - arm / 2, _dpadRadius - inner, arm); break;
        case 2: r = CGRectMake(_dpadCenter.x - arm / 2, _dpadCenter.y + inner, arm, _dpadRadius - inner); break;
        default: r = CGRectMake(_dpadCenter.x - _dpadRadius, _dpadCenter.y - arm / 2, _dpadRadius - inner, arm); break;
        }
        _dpad[i].frame = r;
        _dpad[i].lineWidth = 1.5;
        UIBezierPath *p = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(0, 0, r.size.width, r.size.height) cornerRadius:7 * u];
        const CGPoint c = CGPointMake(r.size.width / 2, r.size.height / 2), d = kDirection[i], side = CGPointMake(-d.y, d.x);
        const CGFloat a = 7 * u;
        [p moveToPoint:CGPointMake(c.x + d.x * a, c.y + d.y * a)];
        [p addLineToPoint:CGPointMake(c.x - d.x * a * 0.6 + side.x * a, c.y - d.y * a * 0.6 + side.y * a)];
        [p addLineToPoint:CGPointMake(c.x - d.x * a * 0.6 - side.x * a, c.y - d.y * a * 0.6 - side.y * a)];
        [p closePath];
        _dpad[i].path = p.CGPath;
    }

    // face buttons with their symbols
    for (int i = 0; i < 4; ++i) {
        const CGPoint c = CGPointMake(_faceCenter.x + kDirection[i].x * _faceOffset, _faceCenter.y + kDirection[i].y * _faceOffset);
        const CGFloat r = _faceRadius, m = r * 0.48;
        _face[i].frame = CGRectMake(c.x - r, c.y - r, 2 * r, 2 * r);
        _face[i].lineWidth = 1.5;
        _face[i].path = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(0, 0, 2 * r, 2 * r)].CGPath;
        _faceMark[i].frame = _face[i].bounds;
        _faceMark[i].lineWidth = 2.2 * u;
        UIBezierPath *mark;
        switch (i) {
        case 0:
            mark = [UIBezierPath bezierPath];
            [mark moveToPoint:CGPointMake(r, r - m)];
            [mark addLineToPoint:CGPointMake(r + m * 0.95, r + m * 0.65)];
            [mark addLineToPoint:CGPointMake(r - m * 0.95, r + m * 0.65)];
            [mark closePath];
            break;
        case 1: mark = [UIBezierPath bezierPathWithOvalInRect:CGRectMake(r - m, r - m, 2 * m, 2 * m)]; break;
        case 2:
            mark = [UIBezierPath bezierPath];
            [mark moveToPoint:CGPointMake(r - m * 0.8, r - m * 0.8)];
            [mark addLineToPoint:CGPointMake(r + m * 0.8, r + m * 0.8)];
            [mark moveToPoint:CGPointMake(r + m * 0.8, r - m * 0.8)];
            [mark addLineToPoint:CGPointMake(r - m * 0.8, r + m * 0.8)];
            break;
        default: mark = [UIBezierPath bezierPathWithRect:CGRectMake(r - m * 0.8, r - m * 0.8, 1.6 * m, 1.6 * m)]; break;
        }
        _faceMark[i].path = mark.CGPath;
    }

    _stickBase.frame = CGRectMake(_stickCenter.x - _stickRadius, _stickCenter.y - _stickRadius, 2 * _stickRadius, 2 * _stickRadius);
    _stickBase.lineWidth = 1.5;
    _stickBase.path = [UIBezierPath bezierPathWithOvalInRect:_stickBase.bounds].CGPath;
    _stickKnob.bounds = CGRectMake(0, 0, 2 * _knobRadius, 2 * _knobRadius);
    _stickKnob.lineWidth = 1.5;
    _stickKnob.path = [UIBezierPath bezierPathWithOvalInRect:_stickKnob.bounds].CGPath;
    [self placeKnob];
    [self restyle];
}

- (void)placeKnob {
    _stickKnob.position = CGPointMake(_stickCenter.x + _stick.x * _stickRadius, _stickCenter.y + _stick.y * _stickRadius);
}

// ---- touches

- (std::uint16_t)faceAt:(CGPoint)p {
    std::uint16_t mask = 0;
    int nearest = 0;
    CGFloat best = 1e9;
    for (int i = 0; i < 4; ++i) {
        const CGPoint c = CGPointMake(_faceCenter.x + kDirection[i].x * _faceOffset, _faceCenter.y + kDirection[i].y * _faceOffset);
        const CGFloat d = distance(p, c);
        if (d < _faceRadius + 7 * _u) mask |= kFaceMask[i];
        if (d < best) best = d, nearest = i;
    }
    return mask ? mask : kFaceMask[nearest];
}

- (std::uint16_t)dpadAt:(CGPoint)p {
    const CGFloat dx = p.x - _dpadCenter.x, dy = p.y - _dpadCenter.y, len = std::hypot(dx, dy);
    if (len < _dpadRadius * 0.2) return 0;
    std::uint16_t mask = 0;
    const CGFloat edge = len * 0.38;   // sin(22.5 degrees): eight directions of equal size
    if (dy < -edge) mask |= kUp;
    if (dy > edge) mask |= kDown;
    if (dx < -edge) mask |= kLeft;
    if (dx > edge) mask |= kRight;
    return mask;
}

- (Track)trackFor:(CGPoint)p {
    Track t;
    for (std::size_t i = 0; i < _keys.size(); ++i)
        if ((!_menuOnly || !_keys[i].mask) && CGRectContainsPoint(CGRectInset(_keys[i].rect, -6 * _u, -6 * _u), p)) {
            t.kind = Kind::key;
            t.key = i;
            t.mask = _keys[i].mask;
            return t;
        }
    if (_menuOnly) return t;
    if (distance(p, _faceCenter) < _faceOffset + _faceRadius + 16 * _u) {
        t.kind = Kind::face;
        t.mask = [self faceAt:p];
        return t;
    }
    if (distance(p, _dpadCenter) < _dpadRadius + 18 * _u) {
        t.kind = Kind::dpad;
        t.mask = [self dpadAt:p];
        return t;
    }
    // the stick takes any touch near it, and anything low on the left side of the screen
    if (!_stickTouch && (distance(p, _stickCenter) < _stickRadius + 40 * _u ||
                         (p.x < self.bounds.size.width * 0.4 && p.y > _dpadCenter.y + _dpadRadius + 18 * _u))) {
        t.kind = Kind::stick;
        return t;
    }
    return t;
}

- (void)moveStick:(CGPoint)p {
    CGFloat x = (p.x - _stickCenter.x) / _stickRadius, y = (p.y - _stickCenter.y) / _stickRadius;
    const CGFloat len = std::hypot(x, y);
    if (len > 1) x /= len, y /= len;
    _stick = CGPointMake(x, y);
}

- (void)update:(Track &)t at:(CGPoint)p {
    switch (t.kind) {
    case Kind::face: t.mask = [self faceAt:p]; break;
    case Kind::dpad: t.mask = [self dpadAt:p]; break;
    case Kind::key: t.mask = CGRectContainsPoint(CGRectInset(_keys[t.key].rect, -14 * _u, -14 * _u), p) ? _keys[t.key].mask : 0; break;
    case Kind::stick: [self moveStick:p]; break;
    default: break;
    }
}

- (void)publish {
    std::uint16_t buttons = 0;
    for (const auto &[touch, t] : _tracks) buttons |= t.mask;
    const CFTimeInterval now = CACurrentMediaTime();
    CFTimeInterval wait = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const std::uint16_t m = static_cast<std::uint16_t>(1u << bit);
        if ((buttons & m) && !(_held & m)) _pressedAt[bit] = now;   // a new press
        else if (!(buttons & m) && (_held & m) && now - _pressedAt[bit] < kMinimumPress) {   // let go too soon: keep it a moment
            buttons |= m;
            wait = std::max(wait, kMinimumPress - (now - _pressedAt[bit]));
        }
    }
    _held = buttons;
    if (wait > 0) {
        __weak TouchControls *weakSelf = self;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(wait * NSEC_PER_SEC) + 1000000), dispatch_get_main_queue(), ^{ [weakSelf publish]; });
    }
    if (!_stickTouch) _stick = CGPointZero;
    const auto axis = [](CGFloat v) { return static_cast<std::uint16_t>(std::clamp(static_cast<int>(std::lround((v + 1.0) * 127.5)), 0, 255)); };
    g_touch_buttons.store(buttons);
    g_touch_stick.store(_stickTouch ? static_cast<std::uint16_t>(axis(_stick.x) | (axis(_stick.y) << 8)) : 0x8080);
    _lit = buttons;
    [self placeKnob];
    [self restyle];
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        const CGPoint p = [touch locationInView:self];
        Track t = [self trackFor:p];
        if (t.kind == Kind::none) continue;
        if (t.kind == Kind::stick) {
            _stickTouch = (__bridge const void *)touch;
            [self moveStick:p];
        }
        _tracks[(__bridge const void *)touch] = t;
    }
    [self publish];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        const auto it = _tracks.find((__bridge const void *)touch);
        if (it != _tracks.end()) [self update:it->second at:[touch locationInView:self]];
    }
    [self publish];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    bool menu = false;
    for (UITouch *touch in touches) {
        const void *key = (__bridge const void *)touch;
        const auto it = _tracks.find(key);
        if (it == _tracks.end()) continue;
        const Track &t = it->second;
        if (t.kind == Kind::key && _keys[t.key].mask == 0 &&
            CGRectContainsPoint(CGRectInset(_keys[t.key].rect, -14 * _u, -14 * _u), [touch locationInView:self]))
            menu = true;
        if (key == _stickTouch) _stickTouch = nullptr;
        _tracks.erase(it);
    }
    [self publish];
    if (menu && _onMenu) _onMenu();
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    for (UITouch *touch in touches) {
        const void *key = (__bridge const void *)touch;
        if (key == _stickTouch) _stickTouch = nullptr;
        _tracks.erase(key);
    }
    [self publish];
}

- (void)releaseAll {
    _tracks.clear();
    _stickTouch = nullptr;
    [self publish];
}

@end
