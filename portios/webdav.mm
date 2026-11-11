// Memory card sync with your own WebDAV server: see webdav.h.
#import "webdav.h"

#import <CommonCrypto/CommonDigest.h>
#import <Security/Security.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

NSString *const kUrlKey = @"sync.url", *const kUserKey = @"sync.user", *const kInsecureKey = @"sync.insecure";
NSString *const kVersionKey = @"sync.version";   // the server's version (ETag) of the card at the last sync
NSString *const kHashKey = @"sync.hash";         // SHA-256 of this device's card at the last sync
NSString *const kKeychainService = @"p3fes.webdav";

// for each save state slot: the server's version at the last sync, and this device's file (date and size) at that time
NSString *state_key(int slot, NSString *what) { return [NSString stringWithFormat:@"sync.state%d.%@", slot, what]; }

NSDate *http_date(NSHTTPURLResponse *r) {
    NSString *text = [r valueForHTTPHeaderField:@"Last-Modified"];
    if (!text.length) return nil;
    NSDateFormatter *f = [[NSDateFormatter alloc] init];
    f.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    f.timeZone = [NSTimeZone timeZoneWithAbbreviation:@"GMT"];
    f.dateFormat = @"EEE, dd MMM yyyy HH:mm:ss zzz";
    return [f dateFromString:text];
}

NSString *sha256(NSData *data) {
    if (!data) return @"";
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), digest);
    NSMutableString *hex = [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (unsigned char b : digest) [hex appendFormat:@"%02x", b];
    return hex;
}

// Where the password goes when the keychain refuses it (an app signed without an application identifier, such as a Simulator
// build): the app's private Library folder, which the Files app does not show.
NSString *password_file() {
    NSString *dir = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir stringByAppendingPathComponent:@"sync-password"];
}

NSMutableDictionary *keychain_query(NSString *account) {
    return [@{(__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword, (__bridge id)kSecAttrService : kKeychainService,
              (__bridge id)kSecAttrAccount : account ?: @""} mutableCopy];
}

// what identifies one version of the file on the server: its ETag, or else its date and size
NSString *version_of(NSHTTPURLResponse *r) {
    NSString *etag = [r valueForHTTPHeaderField:@"ETag"];
    if (etag.length) return etag;
    NSString *date = [r valueForHTTPHeaderField:@"Last-Modified"], *size = [r valueForHTTPHeaderField:@"Content-Length"];
    return date.length ? [NSString stringWithFormat:@"%@|%@", date, size ?: @""] : @"";
}

bool strong_etag(NSString *v) { return [v hasPrefix:@"\""]; }   // If-Match needs a strong ETag

NSString *describe(NSInteger status, NSError *error) {
    if (error) return error.localizedDescription;
    switch (status) {
    case 401: return @"wrong user name or password";
    case 403: return @"the server refused access (403)";
    case 404: return @"not found (404)";
    case 405: return @"the server does not allow this (405); is the address a WebDAV folder?";
    case 409: return @"the folder above it does not exist on the server (409)";
    case 507: return @"the server is full (507)";
    default: return [NSString stringWithFormat:@"the server answered %ld", static_cast<long>(status)];
    }
}

}

@interface SaveSync () <NSURLSessionTaskDelegate>
@end

@implementation SaveSync {
    NSURLSession *_session;
    NSURLSession *_bulk;                     // save states: bigger files, more time
    NSTimer *_uploadTimer;
    BOOL _busy, _again, _paused, _questionOpen;
    NSMutableOrderedSet<NSNumber *> *_stateQueue;   // slots waiting to be uploaded
    BOOL _stateUploading;
}

+ (instancetype)shared {
    static SaveSync *s;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ s = [[SaveSync alloc] init]; });
    return s;
}

- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    config.timeoutIntervalForRequest = 20;
    config.timeoutIntervalForResource = 60;
    config.requestCachePolicy = NSURLRequestReloadIgnoringLocalAndRemoteCacheData;
    config.URLCache = nil;
    config.HTTPCookieStorage = nil;
    _session = [NSURLSession sessionWithConfiguration:config delegate:self delegateQueue:NSOperationQueue.mainQueue];
    NSURLSessionConfiguration *bulk = [config copy];
    bulk.timeoutIntervalForRequest = 60;
    bulk.timeoutIntervalForResource = 15 * 60;
    _bulk = [NSURLSession sessionWithConfiguration:bulk delegate:self delegateQueue:NSOperationQueue.mainQueue];
    _stateQueue = [NSMutableOrderedSet orderedSet];
    return self;
}

// ---- settings

- (NSString *)url { return [NSUserDefaults.standardUserDefaults stringForKey:kUrlKey] ?: @""; }
- (NSString *)user { return [NSUserDefaults.standardUserDefaults stringForKey:kUserKey] ?: @""; }
- (BOOL)insecure { return [NSUserDefaults.standardUserDefaults boolForKey:kInsecureKey]; }
- (BOOL)configured { return self.fileURL != nil; }

- (NSString *)password {
    NSMutableDictionary *q = keychain_query(self.user);
    q[(__bridge id)kSecReturnData] = @YES;
    q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
    CFTypeRef found = nullptr;
    NSData *data = nil;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &found) == errSecSuccess && found) data = (__bridge_transfer NSData *)found;
    else data = [NSData dataWithContentsOfFile:password_file()];
    return data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"" : @"";
}

- (void)setURL:(NSString *)url user:(NSString *)user password:(NSString *)password insecure:(BOOL)insecure {
    url = [url stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    user = [user stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    if (![url isEqualToString:self.url]) {   // another server: nothing is known about its copy yet
        [d removeObjectForKey:kVersionKey];
        [d removeObjectForKey:kHashKey];
        for (int slot = 1; slot <= kSaveSyncSlots; ++slot) {
            [d removeObjectForKey:state_key(slot, @"version")];
            [d removeObjectForKey:state_key(slot, @"local")];
        }
    }
    SecItemDelete((__bridge CFDictionaryRef)keychain_query(self.user));
    [NSFileManager.defaultManager removeItemAtPath:password_file() error:nil];
    [d setObject:url forKey:kUrlKey];
    [d setObject:user forKey:kUserKey];
    [d setBool:insecure forKey:kInsecureKey];
    if (password.length) {
        NSMutableDictionary *q = keychain_query(user);
        q[(__bridge id)kSecValueData] = [password dataUsingEncoding:NSUTF8StringEncoding];
        q[(__bridge id)kSecAttrAccessible] = (__bridge id)kSecAttrAccessibleAfterFirstUnlock;
        const OSStatus added = SecItemAdd((__bridge CFDictionaryRef)q, nullptr);
        if (added != errSecSuccess) {
            std::fprintf(stderr, "[sync] the keychain refused the password (%d); keeping it in the app's private folder\n", static_cast<int>(added));
            [[password dataUsingEncoding:NSUTF8StringEncoding] writeToFile:password_file()
                                                                   options:NSDataWritingAtomic | NSDataWritingFileProtectionCompleteUntilFirstUserAuthentication
                                                                     error:nil];
        }
    }
    _paused = NO;
}

- (NSURL *)fileURL {
    NSString *s = self.url;
    if (!s.length) return nil;
    if (![s containsString:@"://"]) s = [@"https://" stringByAppendingString:s];
    NSURL *u = [NSURL URLWithString:s];
    if (!u.host.length || !([u.scheme isEqualToString:@"http"] || [u.scheme isEqualToString:@"https"])) return nil;
    if ([u.path.lowercaseString hasSuffix:@".ps2"]) return u;
    if (![s hasSuffix:@"/"]) s = [s stringByAppendingString:@"/"];
    return [NSURL URLWithString:[s stringByAppendingString:@"memcard.ps2"]];
}

- (NSString *)remembered:(NSString *)key { return [NSUserDefaults.standardUserDefaults stringForKey:key] ?: @""; }

- (void)rememberVersion:(NSString *)version hash:(NSString *)hash {
    [NSUserDefaults.standardUserDefaults setObject:version ?: @"" forKey:kVersionKey];
    [NSUserDefaults.standardUserDefaults setObject:hash ?: @"" forKey:kHashKey];
}

- (void)say:(NSString *)message {
    std::fprintf(stderr, "[sync] %s\n", message.UTF8String);
    if (_status) _status(message);
}

// ---- requests

- (NSMutableURLRequest *)request:(NSString *)method url:(NSURL *)url {
    NSMutableURLRequest *r = [NSMutableURLRequest requestWithURL:url cachePolicy:NSURLRequestReloadIgnoringLocalAndRemoteCacheData timeoutInterval:20];
    r.HTTPMethod = method;
    // sent straight away: most WebDAV servers take Basic, and a Digest one answers 401 and gets the password through the delegate
    if (self.user.length) {
        NSString *pair = [NSString stringWithFormat:@"%@:%@", self.user, self.password];
        [r setValue:[@"Basic " stringByAppendingString:[[pair dataUsingEncoding:NSUTF8StringEncoding] base64EncodedStringWithOptions:0]]
            forHTTPHeaderField:@"Authorization"];
    }
    return r;
}

- (void)send:(NSURLRequest *)r done:(void (^)(NSInteger status, NSHTTPURLResponse *response, NSData *data, NSError *error))done {
    [[_session dataTaskWithRequest:r completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
        done(http ? http.statusCode : 0, http, data, error);
    }] resume];
}

- (void)head:(void (^)(NSInteger status, NSString *version, NSError *error))done {
    [self send:[self request:@"HEAD" url:self.fileURL] done:^(NSInteger status, NSHTTPURLResponse *r, NSData *, NSError *error) {
        done(status, r ? version_of(r) : @"", error);
    }];
}

- (void)get:(void (^)(NSInteger status, NSData *data, NSString *version, NSError *error))done {
    [self send:[self request:@"GET" url:self.fileURL] done:^(NSInteger status, NSHTTPURLResponse *r, NSData *data, NSError *error) {
        done(status, data, r ? version_of(r) : @"", error);
    }];
}

// creates the folder (an error just means it is there already, or the address is a file), then sends the card. ifMatch: the
// version it replaces (412 if the server has another), "*new" for none, nil for no check
- (void)put:(NSData *)data ifMatch:(NSString *)ifMatch done:(void (^)(NSInteger status, NSString *version, NSError *error))done {
    NSURL *file = self.fileURL;
    void (^upload)(void) = ^{
        NSMutableURLRequest *r = [self request:@"PUT" url:file];
        r.HTTPBody = data;
        [r setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
        if ([ifMatch isEqualToString:@"*new"]) [r setValue:@"*" forHTTPHeaderField:@"If-None-Match"];
        else if (ifMatch && strong_etag(ifMatch)) [r setValue:ifMatch forHTTPHeaderField:@"If-Match"];
        [self send:r done:^(NSInteger status, NSHTTPURLResponse *response, NSData *, NSError *error) {
            if (error || status < 200 || status >= 300) return done(status, @"", error);
            NSString *etag = [response valueForHTTPHeaderField:@"ETag"];
            if (etag.length) return done(status, etag, nil);
            [self head:^(NSInteger, NSString *version, NSError *) { done(status, version, nil); }];   // the server did not say: ask
        }];
    };
    NSURL *folder = file.URLByDeletingLastPathComponent;
    if (!folder.path.length || [folder.path isEqualToString:@"/"]) return upload();
    [self send:[self request:@"MKCOL" url:folder] done:^(NSInteger, NSHTTPURLResponse *, NSData *, NSError *) { upload(); }];
}

- (void)URLSession:(NSURLSession *)session
                   task:(NSURLSessionTask *)task
    didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
      completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completion {
    NSString *method = challenge.protectionSpace.authenticationMethod;
    if ([method isEqualToString:NSURLAuthenticationMethodServerTrust]) {
        if (self.insecure && challenge.protectionSpace.serverTrust)
            return completion(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:challenge.protectionSpace.serverTrust]);
        return completion(NSURLSessionAuthChallengePerformDefaultHandling, nil);
    }
    if (challenge.previousFailureCount == 0 && self.user.length &&
        ([method isEqualToString:NSURLAuthenticationMethodHTTPBasic] || [method isEqualToString:NSURLAuthenticationMethodHTTPDigest] ||
         [method isEqualToString:NSURLAuthenticationMethodNTLM] || [method isEqualToString:NSURLAuthenticationMethodDefault]))
        return completion(NSURLSessionAuthChallengeUseCredential,
                          [NSURLCredential credentialWithUser:self.user password:self.password persistence:NSURLCredentialPersistenceNone]);
    completion(NSURLSessionAuthChallengePerformDefaultHandling, nil);   // the 401 then comes back as the answer
}

// ---- the card on this device

- (NSData *)readCard { return [NSData dataWithContentsOfFile:_cardPath]; }

- (BOOL)writeCard:(NSData *)data {
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *bak = [_cardPath stringByAppendingString:@".bak"];
    if ([fm fileExistsAtPath:_cardPath]) {
        [fm removeItemAtPath:bak error:nil];
        [fm copyItemAtPath:_cardPath toPath:bak error:nil];
    }
    return [data writeToFile:_cardPath atomically:YES];
}

- (void)keepServerCopy:(NSData *)data {
    if (!data.length) return;
    NSString *path = [_cardPath.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"memcard-server.ps2.bak"];
    [data writeToFile:path atomically:YES];
}

- (void)ask:(NSString *)title message:(NSString *)message choices:(NSArray<NSString *> *)choices done:(void (^)(NSInteger choice))done {
    UIViewController *vc = _presenter;
    while (vc.presentedViewController) vc = vc.presentedViewController;
    if (!vc) return done(choices.count - 1);
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title message:message preferredStyle:UIAlertControllerStyleAlert];
    for (NSUInteger i = 0; i < choices.count; ++i)
        [a addAction:[UIAlertAction actionWithTitle:choices[i]
                                              style:i + 1 == choices.count ? UIAlertActionStyleCancel : UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *) {
                                                if (self->_asking) self->_asking(NO);
                                                done(static_cast<NSInteger>(i));
                                            }]];
    if (_asking) _asking(YES);
    [vc presentViewController:a animated:YES completion:nil];
}

// ---- before the game starts

- (void)syncAtStart:(void (^)(void))done {
    if (!self.configured) return done();
    [self say:@"Syncing saves with your server..."];
    NSData *local = [self readCard];
    NSString *localHash = sha256(local), *lastHash = [self remembered:kHashKey], *lastVersion = [self remembered:kVersionKey];
    [self head:^(NSInteger status, NSString *version, NSError *error) {
        if (error || (status != 200 && status != 404)) {
            [self say:[NSString stringWithFormat:@"Save sync: %@. Playing with this device's save.", describe(status, error)]];
            return done();
        }
        if (status == 404) {   // nothing on the server yet
            if (!local) return done();
            return [self put:local ifMatch:@"*new" done:^(NSInteger st, NSString *v, NSError *e) {
                if (st >= 200 && st < 300) {
                    [self rememberVersion:v hash:localHash];
                    [self say:@"Save sync: your save is now on the server."];
                } else {
                    [self say:[NSString stringWithFormat:@"Save sync: cannot upload (%@).", describe(st, e)]];
                }
                done();
            }];
        }
        const bool localChanged = !local || ![localHash isEqualToString:lastHash];
        const bool serverChanged = !lastVersion.length || ![version isEqualToString:lastVersion];
        if (local && !localChanged && !serverChanged) {
            [self say:@"Save sync: up to date."];
            return done();
        }
        if (local && localChanged && !serverChanged) {   // only this device saved since the last sync
            return [self put:local ifMatch:version done:^(NSInteger st, NSString *v, NSError *e) {
                if (st >= 200 && st < 300) {
                    [self rememberVersion:v hash:localHash];
                    [self say:@"Save sync: uploaded this device's newer save."];
                } else {
                    [self say:[NSString stringWithFormat:@"Save sync: cannot upload (%@).", describe(st, e)]];
                }
                done();
            }];
        }
        [self get:^(NSInteger st, NSData *remote, NSString *v, NSError *e) {
            if (e || st != 200 || !remote.length) {
                [self say:[NSString stringWithFormat:@"Save sync: cannot download (%@). Playing with this device's save.", describe(st, e)]];
                return done();
            }
            NSString *remoteHash = sha256(remote);
            if (!local || [remoteHash isEqualToString:localHash] || !localChanged) {   // nothing here, the same card, or only the server changed
                if (![remoteHash isEqualToString:localHash] && ![self writeCard:remote]) {
                    [self say:@"Save sync: cannot write the card on this device."];
                    return done();
                }
                [self rememberVersion:v hash:remoteHash];
                [self say:[remoteHash isEqualToString:localHash] ? @"Save sync: up to date." : @"Save sync: loaded the newer save from your server."];
                return done();
            }
            // both sides changed, or this device never synced with this server: ask
            self->_questionOpen = YES;
            [self ask:@"Which save?"
                message:@"The save on your server and the save on this device are different, and both may have new progress.\n\n"
                        @"The one you don't pick is kept in the app's folder (memcard.ps2.bak or memcard-server.ps2.bak)."
                choices:@[ @"Use the server's save", @"Keep this device's save", @"Don't sync now" ]
                   done:^(NSInteger choice) {
                       self->_questionOpen = NO;
                       if (choice == 0) {
                           if ([self writeCard:remote]) {
                               [self rememberVersion:v hash:remoteHash];
                               [self say:@"Save sync: loaded the save from your server."];
                           } else {
                               [self say:@"Save sync: cannot write the card on this device."];
                           }
                           return done();
                       }
                       if (choice == 1) {
                           [self keepServerCopy:remote];
                           return [self put:local ifMatch:v done:^(NSInteger st2, NSString *v2, NSError *e2) {
                               if (st2 >= 200 && st2 < 300) {
                                   [self rememberVersion:v2 hash:localHash];
                                   [self say:@"Save sync: this device's save is now on the server."];
                               } else {
                                   [self say:[NSString stringWithFormat:@"Save sync: cannot upload (%@).", describe(st2, e2)]];
                               }
                               done();
                           }];
                       }
                       self->_paused = YES;   // until the next start, where it asks again
                       [self say:@"Save sync is off until the app starts again."];
                       done();
                   }];
        }];
    }];
}

// ---- while the game runs

- (void)cardWritten {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!self.configured || self->_paused) return;
        [self->_uploadTimer invalidate];
        self->_uploadTimer = [NSTimer scheduledTimerWithTimeInterval:2.0 repeats:NO block:^(NSTimer *) { [self uploadNow]; }];
    });
}

- (void)uploadNow {
    [_uploadTimer invalidate];
    _uploadTimer = nil;
    if (!self.configured || _paused || _questionOpen) return;
    if (_busy) {
        _again = YES;
        return;
    }
    NSData *local = [self readCard];
    NSString *localHash = sha256(local);
    if (!local || [localHash isEqualToString:[self remembered:kHashKey]]) return;
    _busy = YES;
    UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"save sync" expirationHandler:nil];
    void (^finish)(NSString *) = ^(NSString *message) {
        if (message) [self say:message];
        self->_busy = NO;
        if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
        if (self->_again) {
            self->_again = NO;
            [self uploadNow];
        }
    };
    void (^send)(NSString *) = ^(NSString *ifMatch) {
        [self put:local ifMatch:ifMatch done:^(NSInteger st, NSString *v, NSError *e) {
            if (st >= 200 && st < 300) {
                [self rememberVersion:v hash:localHash];
                return finish(@"Save uploaded to your server.");
            }
            if (st == 412) return finish(@"Save sync: the server's save changed meanwhile; it will ask at the next save.");
            finish([NSString stringWithFormat:@"Save sync: cannot upload (%@). It will try again at the next save.", describe(st, e)]);
        }];
    };
    NSString *lastVersion = [self remembered:kVersionKey];
    [self head:^(NSInteger status, NSString *version, NSError *error) {
        if (status == 404) return send(@"*new");
        if (error || status != 200) return finish([NSString stringWithFormat:@"Save sync: %@. It will try again at the next save.", describe(status, error)]);
        if ([version isEqualToString:lastVersion]) return send(version);
        // another device (or a first sync from here) put a card there since this device last synced
        self->_questionOpen = YES;
        [self ask:@"Replace the server's save?"
            message:@"Your server has a save this device hasn't synced (from another device, or from before sync was set up here). "
                    @"The game is running with this device's save, so it can't switch now.\n\n"
                    @"Replacing keeps the server's old card in the app's folder as memcard-server.ps2.bak."
            choices:@[ @"Replace it with this device's save", @"Keep the server's save" ]
               done:^(NSInteger choice) {
                   self->_questionOpen = NO;
                   if (choice == 0) {
                       return [self get:^(NSInteger st, NSData *remote, NSString *v, NSError *) {
                           if (st == 200) [self keepServerCopy:remote];
                           send(st == 200 ? v : nil);
                       }];
                   }
                   self->_paused = YES;
                   finish(@"Save sync is off until the app starts again. Choose \"Use the server's save\" in the menu to play with it.");
               }];
    }];
}

- (void)test:(void (^)(NSString *))done {
    if (!self.configured) return done(@"Enter a server address first, for example https://example.com/dav/p3fes/");
    [self head:^(NSInteger status, NSString *version, NSError *error) {
        if (status == 200) return done([NSString stringWithFormat:@"Connected. The server has a save at\n%@", self.fileURL.absoluteString]);
        if (status == 404) return done([NSString stringWithFormat:@"Connected. No save on the server yet; the next save goes to\n%@", self.fileURL.absoluteString]);
        done([NSString stringWithFormat:@"Cannot use the server: %@", describe(status, error)]);
    }];
}

- (void)useServerCopyAndQuit {
    if (!self.configured) return;
    [self say:@"Downloading the server's save..."];
    _paused = YES;
    [self get:^(NSInteger st, NSData *remote, NSString *v, NSError *e) {
        if (st != 200 || !remote.length) {
            self->_paused = NO;
            return [self say:[NSString stringWithFormat:@"Save sync: cannot download (%@).", describe(st, e)]];
        }
        if (![self writeCard:remote]) {
            self->_paused = NO;
            return [self say:@"Save sync: cannot write the card on this device."];
        }
        [self rememberVersion:v hash:sha256(remote)];
        std::fprintf(stderr, "[sync] loaded the server's card; closing so the game starts with it\n");
        std::exit(0);   // the running game holds the old card in memory; the next start reads the new one
    }];
}

// ---- save states

- (NSString *)statePath:(int)slot { return [_statesDir stringByAppendingPathComponent:[NSString stringWithFormat:@"slot-%d.state", slot]]; }

- (NSURL *)stateURL:(int)slot {
    NSURL *folder = [self.fileURL.URLByDeletingLastPathComponent URLByAppendingPathComponent:@"states" isDirectory:YES];
    return [folder URLByAppendingPathComponent:[NSString stringWithFormat:@"slot-%d.state.deflate", slot]];
}

// one version of a slot on this device: its date and size ("" when the slot is empty)
- (NSString *)localSignature:(int)slot {
    NSDictionary *a = [NSFileManager.defaultManager attributesOfItemAtPath:[self statePath:slot] error:nil];
    if (!a) return @"";
    return [NSString stringWithFormat:@"%.0f|%llu", a.fileModificationDate.timeIntervalSince1970, a.fileSize];
}

- (void)rememberState:(int)slot version:(NSString *)version local:(NSString *)local {
    [NSUserDefaults.standardUserDefaults setObject:version ?: @"" forKey:state_key(slot, @"version")];
    [NSUserDefaults.standardUserDefaults setObject:local ?: @"" forKey:state_key(slot, @"local")];
}

- (void)stateSaved:(int)slot {
    if (!self.configured || !_statesDir.length || slot < 1 || slot > kSaveSyncSlots) return;
    [_stateQueue addObject:@(slot)];
    [self nextStateUpload];
}

// one slot at a time: compressed off the main thread, then sent
- (void)nextStateUpload {
    if (_stateUploading || !_stateQueue.count) return;
    const int slot = _stateQueue.firstObject.intValue;
    [_stateQueue removeObjectAtIndex:0];
    NSString *path = [self statePath:slot], *local = [self localSignature:slot];
    if (!local.length) return [self nextStateUpload];
    _stateUploading = YES;
    __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"state sync" expirationHandler:^{
        [UIApplication.sharedApplication endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
    }];
    void (^finish)(NSString *) = ^(NSString *message) {
        if (message) [self say:message];
        self->_stateUploading = NO;
        if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
        [self nextStateUpload];
    };
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSData *raw = [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil];
        NSData *packed = [raw compressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!packed.length || ![[self localSignature:slot] isEqualToString:local]) return finish(nil);   // gone or saved again (queued)
            NSURL *url = [self stateURL:slot];
            // the folders may not be there yet: an error just means they are
            [self send:[self request:@"MKCOL" url:url.URLByDeletingLastPathComponent.URLByDeletingLastPathComponent] done:^(NSInteger, NSHTTPURLResponse *, NSData *, NSError *) {
                [self send:[self request:@"MKCOL" url:url.URLByDeletingLastPathComponent] done:^(NSInteger, NSHTTPURLResponse *, NSData *, NSError *) {
                    NSMutableURLRequest *r = [self request:@"PUT" url:url];
                    r.timeoutInterval = 60;
                    [r setValue:@"application/octet-stream" forHTTPHeaderField:@"Content-Type"];
                    [[self->_bulk uploadTaskWithRequest:r fromData:packed completionHandler:^(NSData *, NSURLResponse *response, NSError *error) {
                        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
                        const NSInteger status = http ? http.statusCode : 0;
                        if (error || status < 200 || status >= 300)
                            return finish([NSString stringWithFormat:@"Slot %d couldn't be uploaded (%@). It is sent again the next time you open the "
                                                                     @"state list.", slot, describe(status, error)]);
                        void (^remember)(NSString *) = ^(NSString *version) {
                            [self rememberState:slot version:version local:local];
                            finish([NSString stringWithFormat:@"Slot %d is on your server too.", slot]);
                        };
                        NSString *etag = [http valueForHTTPHeaderField:@"ETag"];
                        if (etag.length) return remember(etag);
                        [self send:[self request:@"HEAD" url:url] done:^(NSInteger, NSHTTPURLResponse *h, NSData *, NSError *) { remember(h ? version_of(h) : @""); }];
                    }] resume];
                }];
            }];
        });
    });
}

- (void)checkStates:(void (^)(NSDictionary<NSNumber *, NSDate *> *))done {
    if (!self.configured || !_statesDir.length) return done(@{});
    NSMutableDictionary<NSNumber *, NSDate *> *onServer = [NSMutableDictionary dictionary];
    __block int left = kSaveSyncSlots;
    __block BOOL answered = NO;
    void (^end)(void) = ^{
        if (answered) return;
        answered = YES;
        done([onServer copy]);
    };
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), end);   // a slow server: go on without it
    for (int slot = 1; slot <= kSaveSyncSlots; ++slot) {
        NSMutableURLRequest *r = [self request:@"HEAD" url:[self stateURL:slot]];
        r.timeoutInterval = 4;
        [self send:r done:^(NSInteger status, NSHTTPURLResponse *response, NSData *, NSError *error) {
            NSString *local = [self localSignature:slot];
            const bool localChanged = local.length && ![local isEqualToString:[self remembered:state_key(slot, @"local")]];
            if (!error && status == 404) {
                if (local.length) [self stateSaved:slot];   // only this device has it
            } else if (!error && status == 200) {
                NSString *version = version_of(response);
                NSDate *serverDate = http_date(response);
                if ([version isEqualToString:[self remembered:state_key(slot, @"version")]]) {
                    if (localChanged) [self stateSaved:slot];   // the server has what this device sent last; this one is newer
                } else {
                    // a copy this device hasn't got (from another device): the newer of the two wins
                    NSDate *localDate = [NSFileManager.defaultManager attributesOfItemAtPath:[self statePath:slot] error:nil].fileModificationDate;
                    if (localChanged && localDate && serverDate && [localDate compare:serverDate] == NSOrderedDescending) [self stateSaved:slot];
                    else if (!answered) onServer[@(slot)] = serverDate ?: NSDate.date;
                }
            }
            if (--left == 0) end();
        }];
    }
}

- (void)fetchState:(int)slot done:(void (^)(NSString *))done {
    if (!self.configured || !_statesDir.length) return done(@"save sync is off");
    NSMutableURLRequest *r = [self request:@"GET" url:[self stateURL:slot]];
    r.timeoutInterval = 60;
    __block UIBackgroundTaskIdentifier task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"state download" expirationHandler:^{
        [UIApplication.sharedApplication endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
    }];
    void (^finish)(NSString *) = ^(NSString *problem) {
        if (task != UIBackgroundTaskInvalid) [UIApplication.sharedApplication endBackgroundTask:task];
        task = UIBackgroundTaskInvalid;
        done(problem);
    };
    [[_bulk dataTaskWithRequest:r completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSHTTPURLResponse *http = [response isKindOfClass:NSHTTPURLResponse.class] ? (NSHTTPURLResponse *)response : nil;
        const NSInteger status = http ? http.statusCode : 0;
        if (error || status != 200 || !data.length) return finish(describe(status, error));
        NSString *version = version_of(http);
        NSDate *serverDate = http_date(http);
        NSString *path = [self statePath:slot];
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
            NSData *raw = [data decompressedDataUsingAlgorithm:NSDataCompressionAlgorithmZlib error:nil];
            const bool good = raw.length > 8 && std::memcmp(raw.bytes, "PS2STATE", 8) == 0;
            [NSFileManager.defaultManager createDirectoryAtPath:self->_statesDir withIntermediateDirectories:YES attributes:nil error:nil];
            const bool written = good && [raw writeToFile:path atomically:YES];
            if (written && serverDate)   // the slot shows when it was saved, not when it came down
                [NSFileManager.defaultManager setAttributes:@{NSFileModificationDate : serverDate} ofItemAtPath:path error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                if (!good) return finish(@"the server's copy is damaged");
                if (!written) return finish(@"it can't be written on this device; is it full?");
                [self rememberState:slot version:version local:[self localSignature:slot]];
                finish(nil);
            });
        });
    }] resume];
}

@end
