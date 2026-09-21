/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright 2026 Austin Lehman
 *
 * schultz_files_ios.m - file dialogs on iOS, which SDL does not provide.
 *
 * Everything here exists because of one fact about the platform: **iOS does
 * not hand an application a path it can open.**
 *
 * On every other platform a file picker answers with a filename, the program
 * calls fopen, and that is the end of it. On iOS the picker answers with a
 * URL pointing outside the application's sandbox, and reading it requires
 * holding a security scope open around every access. A path string on its own
 * is refused. So a backend that only translated the picker's answer into a
 * char * would compile, run, show the right dialog, and hand back a path that
 * fails to open.
 *
 * The three dialogs each deal with that differently, and each one is a
 * decision rather than a detail:
 *
 *   Opening  asks the picker for a copy. UIDocumentPickerViewController with
 *            asCopy:YES puts the chosen file in this application's own
 *            temporary directory and answers with that, which is an ordinary
 *            path in a place we own. It costs a copy of the file and buys an
 *            answer that behaves like every other platform's.
 *
 *   Saving   has no equivalent on iOS at all. There is no "choose somewhere
 *            to write"; there is only "export this file I already have". So
 *            an empty file is made in the temporary directory first and the
 *            picker moves it where the person chose. What comes back is the
 *            file's new home, and the scope for it is held so the host can
 *            write to it the way it would anywhere else.
 *
 *   Folders  cannot be copied, so there is nothing to do but hold the scope.
 *            See schultz_files_ios_hold below for how long, and why.
 *
 * None of this has been run. There is no iOS SDK on the machine this was
 * written on, so it has been read carefully and compiled nowhere. Treat the
 * first run on a device as the first test.
 */

#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <stdlib.h>
#include <string.h>

#include "schultz_files_backend.h"

#if __has_feature(objc_arc)
#error "schultz_files_ios.m is written for manual retain/release; \
compile it without -fobjc-arc."
#endif

/*
 * Security scopes that are kept open for the rest of the session.
 *
 * A folder the person chose is reachable only between
 * startAccessingSecurityScopedResource and its stop, and the scope dies with
 * the URL object. The host is handed a path and told nothing about any of
 * that, so the scope has to outlive the call that produced it.
 *
 * It is held until the process ends. That is a real cost and a small one: it
 * is one URL per folder the person picks, in a session where they are picking
 * folders by hand. The alternative is a way for the host to say "I am done
 * with that one", which is a new call in the public interface, on every
 * platform, for the benefit of one.
 */
static NSMutableArray<NSURL *> *schultz_files_ios_held = nil;

static void schultz_files_ios_hold(NSURL *url)
{
    if (url == nil) {
        return;
    }
    if (schultz_files_ios_held == nil) {
        schultz_files_ios_held = [[NSMutableArray alloc] init];
    }
    if ([url startAccessingSecurityScopedResource]) {
        [schultz_files_ios_held addObject:url];
    }
}

/*
 * The content types a picker will show, worked out from our filters.
 *
 * Our filters are extensions, because that is what every other platform
 * wants: a name and a pattern such as "txt;md". iOS wants UTTypes instead, so
 * each extension is looked up. An extension the system has never heard of
 * yields nil and is dropped rather than guessed at.
 *
 * No filters, or nothing recognised among them, means everything. That
 * matches what the documented default is on the other platforms.
 */
static NSArray<UTType *> *schultz_files_ios_types(
    const schultz_file_filter *filters, uint32_t filter_count)
{
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    uint32_t i;

    for (i = 0; i < filter_count && filters != NULL; i++) {
        const char *pattern = filters[i].pattern;
        const char *at;
        const char *start;

        if (pattern == NULL || strcmp(pattern, "*") == 0) {
            return @[ UTTypeItem ];
        }
        /* "txt;md" is two extensions. Walked by hand because the string is
         * the caller's and must not be written to. */
        start = pattern;
        for (at = pattern; ; at++) {
            if (*at != ';' && *at != '\0') {
                continue;
            }
            if (at > start) {
                NSString *one =
                    [[NSString alloc] initWithBytes:start
                                             length:(NSUInteger)(at - start)
                                           encoding:NSUTF8StringEncoding];
                UTType *type = (one == nil)
                    ? nil : [UTType typeWithFilenameExtension:one];

                if (type != nil) {
                    [types addObject:type];
                }
                [one release];
            }
            if (*at == '\0') {
                break;
            }
            start = at + 1;
        }
    }
    if ([types count] == 0u) {
        return @[ UTTypeItem ];
    }
    return types;
}

/* A name for the file a save dialog is about to export. */
static NSString *schultz_files_ios_save_name(
    const schultz_file_filter *filters, uint32_t filter_count)
{
    NSArray<UTType *> *types = schultz_files_ios_types(filters, filter_count);
    UTType *first = [types firstObject];
    NSString *extension = (first == nil) ? nil
                                         : [first preferredFilenameExtension];

    /*
     * The person renames it in the picker if they want to. There is nothing
     * better to use: schultz_file_options carries no suggested name, because
     * on every other platform the dialog itself is where a name is typed.
     */
    if (extension == nil || [extension length] == 0u) {
        return @"Untitled";
    }
    return [@"Untitled" stringByAppendingPathExtension:extension];
}

/*
 * The delegate, which is also what keeps itself alive.
 *
 * UIKit holds a picker's delegate weakly, so between presenting the dialog
 * and the person answering it nothing else refers to this object. Under
 * manual retain and release that is survivable and exact: the +1 from alloc
 * is never given up at the call site, and is what keeps the delegate around
 * while the dialog is open. finishWithURLs gives it up, which is why every
 * path has to reach there exactly once.
 *
 * Manual retain and release rather than ARC, matching the accessibility
 * backend beside it and the shells in AccessTunnel. The guard above stops
 * this being compiled the other way by accident.
 */
@interface SchultzFilesPicker : NSObject <UIDocumentPickerDelegate>
@property (nonatomic, assign) schultz_files_answer_fn answer;
@property (nonatomic, assign) void *userdata;
@property (nonatomic, assign) schultz_files_kind kind;
@end

@implementation SchultzFilesPicker

/* The one exit. Answers once, then stops keeping itself alive. */
- (void)finishWithURLs:(NSArray<NSURL *> *)urls
{
    NSUInteger count = (urls == nil) ? 0u : [urls count];
    const char **paths = NULL;
    NSUInteger i;

    if (count > 0u) {
        paths = (const char **)calloc(count + 1u, sizeof(*paths));
    }
    if (paths != NULL) {
        NSUInteger kept = 0u;

        for (i = 0; i < count; i++) {
            NSURL *url = [urls objectAtIndex:i];

            /*
             * A copy lands in our own temporary directory and needs no
             * scope. Anything else is the person's own storage, and the
             * path only works while the scope is open.
             */
            if (self.kind != SCHULTZ_FILES_OPEN) {
                schultz_files_ios_hold(url);
            }
            if ([url path] != nil) {
                paths[kept] = [[url path] UTF8String];
                kept++;
            }
        }
        paths[kept] = NULL;
        if (kept == 0u) {
            free(paths);
            paths = NULL;
        }
    }
    /*
     * -1 for the filter: the picker does not report which content type the
     * person was looking at, and the interface says -1 means exactly that.
     */
    /* The cast is needed rather than tidy: C does not add a const to the
     * inner pointer on its own, even though doing so takes nothing away. */
    self.answer(self.userdata, (const char *const *)paths, -1);
    free(paths);
    /* The last reference, taken at alloc. Nothing may touch self after it. */
    [self release];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    (void)controller;
    [self finishWithURLs:urls];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller
{
    (void)controller;
    [self finishWithURLs:nil];
}

@end

/* The view controller a picker has to be presented from. */
static UIViewController *schultz_files_ios_host(SDL_Window *window)
{
    UIWindow *ui = nil;

    if (window != NULL) {
        ui = (__bridge UIWindow *)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL);
    }
    if (ui == nil) {
        return nil;
    }
    return [ui rootViewController];
}

/*
 * The empty file a save dialog exports.
 *
 * iOS moves a file that exists; it does not create one. So one is made here,
 * with nothing in it, and the host writes to wherever it ends up.
 */
static NSURL *schultz_files_ios_placeholder(NSString *name)
{
    NSURL *directory = [NSURL fileURLWithPath:NSTemporaryDirectory()
                                  isDirectory:YES];
    NSURL *file = [directory URLByAppendingPathComponent:name];

    if (![[NSFileManager defaultManager] createFileAtPath:[file path]
                                                 contents:[NSData data]
                                               attributes:nil]) {
        return nil;
    }
    return file;
}

void schultz_files_show(schultz_files_kind kind, SDL_Window *window,
                        const schultz_file_options *options,
                        const schultz_file_filter *filters,
                        uint32_t filter_count,
                        schultz_files_answer_fn answer, void *userdata)
{
    int32_t many;
    char *location = NULL;

    if (answer == NULL) {
        return;
    }
    /*
     * What the options say is read now, not later.
     *
     * The block below runs after this function has returned, and
     * schultz_window.h promises a caller that the options do not have to
     * outlive the call: a host is entitled to build them on the stack and
     * walk away. Every other backend reads them before returning and is fine.
     * This one would be reading freed memory, so it takes what it needs
     * first. The filters are different and may be held: they are the
     * driver's own copy, kept until the answer arrives.
     */
    many = (options != NULL && options->allow_many) ? 1 : 0;
    if (options != NULL && options->location != NULL) {
        size_t length = strlen(options->location);

        location = (char *)malloc(length + 1u);
        if (location != NULL) {
            memcpy(location, options->location, length + 1u);
        }
    }
    /*
     * UIKit is the main thread's, and nothing below may touch it from
     * anywhere else. SDL already runs a host on the main thread on iOS, so
     * this is usually a straight call through; saying it here means it stays
     * correct if that ever stops being true.
     */
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController *host = schultz_files_ios_host(window);
        UIDocumentPickerViewController *picker = nil;
        SchultzFilesPicker *delegate;

        if (host == nil) {
            /* Nothing to present from. The caller is waiting on exactly one
             * answer and has to get it. */
            free(location);
            answer(userdata, NULL, -1);
            return;
        }
        /* The +1 here is kept, not balanced: see the class comment. */
        delegate = [[SchultzFilesPicker alloc] init];
        delegate.answer   = answer;
        delegate.userdata = userdata;
        delegate.kind     = kind;

        switch (kind) {
        case SCHULTZ_FILES_SAVE: {
            NSString *name = schultz_files_ios_save_name(filters,
                                                         filter_count);
            NSURL *placeholder = schultz_files_ios_placeholder(name);

            if (placeholder == nil) {
                [delegate release];
                free(location);
                answer(userdata, NULL, -1);
                return;
            }
            /*
             * asCopy:NO moves the placeholder rather than copying it, which
             * is what leaves this application able to write to the result.
             * A copy would land where the person asked and belong to them
             * alone, and the host's first fopen would be refused.
             */
            picker = [[UIDocumentPickerViewController alloc]
                         initForExportingURLs:@[ placeholder ]
                                       asCopy:NO];
            break;
        }
        case SCHULTZ_FILES_FOLDER:
            picker = [[UIDocumentPickerViewController alloc]
                         initForOpeningContentTypes:@[ UTTypeFolder ]
                                             asCopy:NO];
            break;
        case SCHULTZ_FILES_OPEN:
        default:
            picker = [[UIDocumentPickerViewController alloc]
                         initForOpeningContentTypes:
                             schultz_files_ios_types(filters, filter_count)
                                             asCopy:YES];
            break;
        }
        picker.delegate = delegate;
        if (many && kind == SCHULTZ_FILES_OPEN) {
            picker.allowsMultipleSelection = YES;
        }
        if (location != NULL) {
            picker.directoryURL =
                [NSURL fileURLWithPath:
                    [NSString stringWithUTF8String:location]
                           isDirectory:YES];
        }
        free(location);
        /*
         * The title is not set. UIDocumentPickerViewController does not offer
         * one, and schultz_file_options says a field the platform does not
         * honour is ignored rather than refused.
         */
        [host presentViewController:picker animated:YES completion:nil];
        /* Presenting retains it. The +1 from alloc is ours to give up. */
        [picker release];
    });
}
