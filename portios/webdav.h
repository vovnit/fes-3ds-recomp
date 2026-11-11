// Memory card sync with your own WebDAV server (Nextcloud, ownCloud, Apache or nginx WebDAV, rclone serve webdav, a NAS, ...).
//
// The card is <server folder>/memcard.ps2 (or the exact file, if the address ends in .ps2). The game keeps the card in
// memory once it has started, so the server's copy is only fetched before the game starts; after that every save the
// game writes is sent up a few seconds later. Each device remembers which server version and which card it last synced,
// so it knows which side changed. When both did, it asks which save to keep. The card it replaces stays in the app's
// folder: memcard.ps2.bak for this device's old card, memcard-server.ps2.bak for the server's old card.
//
// Save states (ios_main.mm) are synced too: each slot goes to <server folder>/states/slot-N.state.deflate, compressed with
// raw DEFLATE (about 9 MB instead of 46), when it is saved. When the state list opens, the app asks the server which slots it
// holds a newer copy of (saved on another device); loading one of those downloads it first. For each slot the newest copy
// wins, by date. A slot that could not be uploaded is sent again the next time the list opens.
//
// The address and user name are kept in the app's settings, the password in the iOS keychain.
#pragma once

#import <UIKit/UIKit.h>

@interface SaveSync : NSObject
+ (instancetype)shared;

@property(nonatomic, copy) NSString *cardPath;                   // Documents/memcard.ps2
@property(nonatomic, weak) UIViewController *presenter;          // asks its questions here
@property(nonatomic, copy) void (^status)(NSString *message);    // short messages for the screen (main thread)
@property(nonatomic, copy) void (^asking)(BOOL asking);           // a question is on screen (the game pauses meanwhile)

@property(nonatomic, readonly) BOOL configured;
@property(nonatomic, readonly) NSURL *fileURL;                  // where the card goes on the server
@property(nonatomic, readonly) NSString *url, *user;
@property(nonatomic, readonly) BOOL insecure;                    // accept a self-signed certificate
- (NSString *)password;
- (void)setURL:(NSString *)url user:(NSString *)user password:(NSString *)password insecure:(BOOL)insecure;

- (void)syncAtStart:(void (^)(void))done;       // before the game starts; done is always called, on the main thread
- (void)cardWritten;                            // the game wrote the card (any thread): send it up shortly
- (void)uploadNow;                              // send it up now, if it changed
- (void)test:(void (^)(NSString *result))done;  // checks the address and the password
- (void)useServerCopyAndQuit;                   // fetches the server's card and closes the app, to start with it

// save states: slot-1.state to slot-<kSaveSyncSlots>.state in statesDir
@property(nonatomic, copy) NSString *statesDir;
- (void)stateSaved:(int)slot;                                                   // a slot was saved on this device: send it up
- (void)checkStates:(void (^)(NSDictionary<NSNumber *, NSDate *> *onServer))done;   // slots whose newest copy is on the server,
                                                                                // with its date (empty if sync is off or the
                                                                                // server doesn't answer); done on the main thread
- (void)fetchState:(int)slot done:(void (^)(NSString *problem))done;            // downloads a slot into statesDir; problem is nil
                                                                                // when it worked
@end

constexpr int kSaveSyncSlots = 4;
