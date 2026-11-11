
#if CARROTTUS_PLATFORM_APPLE

/*
 * Carrottus Net HTTP and WebSocket on NSURLSession.
 *
 * Foundation and Security only, so the same file serves macOS, iOS and
 * watchOS - the last without its WebSocket half, which the system doesn't
 * let an ordinary application use there (CRNET_EXT_WEBSOCKET).
 * NSURLSession is callback-driven; each call here starts one task and waits
 * for its callback, which is what makes the API blocking. The callbacks run
 * on the session's own queue, never on the caller's thread.
 *
 * Time limits are kept here, by how long a call waits for its callback, and
 * NSURLSession's own are set out of the way: its request limit counts idle
 * time rather than the whole call, which is not what crnet.h promises.
 * Running out of time, cancel and abort all end the task the same way, by
 * cancelling it, and NSURLSession then calls back as it would for any
 * other failure.
 *
 * TLS: NSURLSession asks its delegate about each server certificate, and
 * that question is where a crnet_tls_desc is applied, with Security's
 * SecTrust doing the evaluating. A desc that asks for nothing is answered
 * "do what you would have done".
 *
 * Plain Objective-C with manual retain/release, like the Metal backend.
 */

#if __has_feature(objc_arc)
#error "crneturlsession.m is written for manual retain/release; compile it without -fobjc-arc."
#endif

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <CommonCrypto/CommonDigest.h>

#include "../../crnetshared.h"
#include "crnet.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "../../../crstructbase.h"

// NSURLSession has no "no limit" value; a year stands in for one.
#define CRNET_APPLE_NO_LIMIT_SECONDS (365.0 * 24.0 * 60.0 * 60.0)

// --------------------------------------------------------------------------
// Instance
// --------------------------------------------------------------------------

/* Neither NSURLSession nor BSD sockets (../posix/crnetsockets.c) has
 * anything to start or stop, so an instance here is only the handle: it
 * exists so callers write one code path across platforms (Winsock needs
 * WSAStartup, libcurl its global init), and so a wait set can tell which
 * objects were made together. */
CR_RESULT crnet_instance_create(
    const crnet_instance_desc *desc,
    crnet_instance *out_instance,
    crnet_error *out_error) {
	crnet_error_clear(out_error);
	const CR_RESULT desc_result = crnet_instance_desc_check(desc, out_instance);
	if (desc_result != CR_SUCCESS)
		return desc_result;

	struct crnet_instance_t *instance = CR_NEW_ZEROED(desc->allocator, struct crnet_instance_t);
	if (!instance)
		return CR_RESULT_OUT_OF_MEMORY;
	instance->head.allocator = *desc->allocator;
	CR_BIRTH(instance);
	CR_QUARANTINE_INIT(&instance->quarantine, &instance->head.allocator);
	*out_instance            = instance;
	return CR_SUCCESS;
}

void crnet_instance_destroy(crnet_instance instance) {
	if (!instance)
		return;
	if (CR_STALE(instance, "crnet_instance_destroy", "instance"))
		return;
	CR_KILL(instance);
	const CR_ALLOCATOR allocator = instance->head.allocator;
	CR_QUARANTINE_RELEASE(&instance->quarantine);
	cr_string_free(&allocator, instance->head.debug_name);
	CR_DELETE(&allocator, struct crnet_instance_t, instance);
}

// --------------------------------------------------------------------------
// Shared by HTTP and WebSocket
// --------------------------------------------------------------------------

/* When a wait that starts now runs out, in the two forms the waits here
 * take. */
#if CRNET_EXT_WEBSOCKET // only a WebSocket's send waits this way
static dispatch_time_t crnet_apple_dispatch_limit(uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return DISPATCH_TIME_FOREVER;
	return dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ms * (int64_t)NSEC_PER_MSEC);
}
#endif

static NSDate *crnet_apple_date_limit(uint32_t timeout_ms) {
	if (timeout_ms == CRNET_WAIT_FOREVER)
		return [NSDate distantFuture];
	return [NSDate dateWithTimeIntervalSinceNow:(NSTimeInterval)timeout_ms / 1000.0];
}

static CR_RESULT crnet_apple_result(NSError *error, CR_RESULT otherwise) {
	// A proxy that can't be reached is reported from CFNetwork's own
	// domain rather than NSURLSession's: 306 and 310 are its "connection to
	// the HTTP / HTTPS proxy failed" (kCFErrorHTTPProxyConnectionFailure,
	// kCFErrorHTTPSProxyConnectionFailure).
	if (error && [error.domain isEqualToString:@"kCFErrorDomainCFNetwork"] && (error.code == 306 || error.code == 310))
		return CR_RESULT_NET_CONNECT_FAILED;
	if (!error || ![error.domain isEqualToString:NSURLErrorDomain])
		return otherwise;
	switch (error.code) {
	case NSURLErrorTimedOut:
		// NSURLSession's own limits are set out of reach, so this is the
		// system's passed up; every wait this file keeps for the caller is
		// ended by its own clock, not by this.
		return CR_RESULT_NET_STACK_TIMED_OUT;
	case NSURLErrorCannotFindHost:
	case NSURLErrorDNSLookupFailed:
		return CR_RESULT_NET_RESOLVE_FAILED;
	case NSURLErrorCannotConnectToHost:
	case NSURLErrorNotConnectedToInternet:
		return CR_RESULT_NET_CONNECT_FAILED;
	// NSURLErrorNetworkConnectionLost is a connection that was made and then
	// broke, which is the calling part's catch-all, not a failure to connect.
	case NSURLErrorSecureConnectionFailed:
	case NSURLErrorServerCertificateHasBadDate:
	case NSURLErrorServerCertificateUntrusted:
	case NSURLErrorServerCertificateHasUnknownRoot:
	case NSURLErrorServerCertificateNotYetValid:
	case NSURLErrorClientCertificateRejected:
	case NSURLErrorClientCertificateRequired:
		return CR_RESULT_NET_TLS_FAILED;
	case NSURLErrorBadURL:
	case NSURLErrorUnsupportedURL:
		return CR_RESULT_NET_INVALID_URL;
	default:
		return otherwise;
	}
}

/* An NSError as a record: its domain decides the source, its code is kept
 * as it is, and its description is the text - led by the domain's name
 * where the domain has no source of its own. Returns `result`. Takes a nil
 * error, which records the result with no source. */
static CR_RESULT crnet_apple_record(crnet_error *record, CR_RESULT result, NSError *error) {
	if (!error)
		return crnet_error_record(record, result, CR_PLATFORM_ERROR_SOURCE_NONE, 0, NULL);

	CR_PLATFORM_ERROR_SOURCE source      = CR_PLATFORM_ERROR_SOURCE_OTHER;
	NSString            *description = error.localizedDescription;
	if ([error.domain isEqualToString:NSURLErrorDomain])
		source = CR_PLATFORM_ERROR_SOURCE_NSURL;
	else if ([error.domain isEqualToString:NSPOSIXErrorDomain])
		source = CR_PLATFORM_ERROR_SOURCE_ERRNO;
	else if ([error.domain isEqualToString:NSOSStatusErrorDomain])
		source = CR_PLATFORM_ERROR_SOURCE_OSSTATUS;
	else
		description = [NSString stringWithFormat:@"%@: %@", error.domain, description];
	return crnet_error_record(record, result, source, (int64_t)error.code, description.UTF8String);
}

/* Everything NSURLSession would otherwise keep or decide between requests,
 * switched off: cookies, stored credentials, the response cache, and its
 * own 60 second / 7 day limits (each call keeps the caller's instead) -
 * and the route `proxy` asks for. Autoreleased; nil when the proxy's host
 * isn't UTF-8. */
static NSURLSessionConfiguration *crnet_apple_configuration(const struct crnet_proxy *proxy) {
	NSURLSessionConfiguration *configuration = [NSURLSessionConfiguration ephemeralSessionConfiguration];
	configuration.HTTPCookieStorage          = nil;
	configuration.HTTPShouldSetCookies       = NO;
	configuration.HTTPCookieAcceptPolicy     = NSHTTPCookieAcceptPolicyNever;
	configuration.URLCredentialStorage       = nil;
	configuration.URLCache                   = nil;
	configuration.requestCachePolicy         = NSURLRequestReloadIgnoringLocalCacheData;
	configuration.timeoutIntervalForRequest  = CRNET_APPLE_NO_LIMIT_SECONDS;
	configuration.timeoutIntervalForResource = CRNET_APPLE_NO_LIMIT_SECONDS;

	// Left alone, the configuration uses the system's proxies. An empty
	// dictionary is no proxy at all; one that names a proxy is that proxy,
	// for plain and TLS connections alike. The keys are CFNetwork's, given
	// by their text because the TLS ones are only declared for macOS.
	if (proxy->mode == CRNET_PROXY_NONE) {
		configuration.connectionProxyDictionary = @{};
	} else if (proxy->mode == CRNET_PROXY_EXPLICIT) {
		NSString *host = [NSString stringWithUTF8String:proxy->host];
		if (!host)
			return nil;
		NSNumber *port                          = @(proxy->port);
		configuration.connectionProxyDictionary = @{
			@"HTTPEnable" : @1,
			@"HTTPProxy" : host,
			@"HTTPPort" : port,
			@"HTTPSEnable" : @1,
			@"HTTPSProxy" : host,
			@"HTTPSPort" : port,
		};
	}
	return configuration;
}

/* A URL that isn't valid UTF-8 or doesn't parse is
 * CR_RESULT_NET_INVALID_URL; a header with no name or value, or one
 * that isn't valid UTF-8, is CR_RESULT_DESC_INVALID_PARAMETERS.
 * `*out_request` is nil after either, and autoreleased otherwise. The
 * request's own limit is set out of the way; see the top of the file. */
static CR_RESULT crnet_apple_request(
    const char *url,
    const crnet_header *headers,
    uint32_t header_count,
    NSMutableURLRequest **out_request) {
	*out_request     = nil;
	NSString *string = [NSString stringWithUTF8String:url];
	if (!string)
		return CR_RESULT_NET_INVALID_URL;
	NSURL *parsed = [NSURL URLWithString:string];
	if (!parsed || !parsed.host)
		return CR_RESULT_NET_INVALID_URL;

	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:parsed];
	request.timeoutInterval      = CRNET_APPLE_NO_LIMIT_SECONDS;
	for (uint32_t i = 0; i < header_count; ++i) {
		if (!headers[i].name || !headers[i].value)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		NSString *name  = [NSString stringWithUTF8String:headers[i].name];
		NSString *value = [NSString stringWithUTF8String:headers[i].value];
		if (!name || !value)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		[request addValue:value forHTTPHeaderField:name];
	}
	*out_request = request;
	return CR_SUCCESS;
}

// --------------------------------------------------------------------------
// TLS
// --------------------------------------------------------------------------

/* A crnet_tls_desc in the forms Security takes it, made once and held by
 * the session's delegate. Not changed after it is made, so the session's
 * queue reads it without a lock. */
@interface CrNetTLS : NSObject {
@public
	CRNET_TLS_TRUST trust;
	BOOL              skip_host_name_check;
	NSArray          *roots; // of SecCertificateRef, or nil
	NSData           *pins;  // CRNET_TLS_PIN_SIZE bytes each, or nil
}
@end

@implementation CrNetTLS

- (void)dealloc {
	[roots release];
	[pins release];
	[super dealloc];
}

@end

/* `*out_tls` is the caller's to release. */
static CR_RESULT crnet_apple_tls_create(const crnet_tls_desc *desc, CrNetTLS **out_tls) {
	*out_tls          = nil;
	CR_RESULT result = crnet_tls_desc_check(desc);
	if (CR_FAILED(result))
		return result;

	CrNetTLS *tls = [[CrNetTLS alloc] init];
	if (!tls)
		return CR_RESULT_OUT_OF_MEMORY;
	tls->trust                = desc->trust;
	tls->skip_host_name_check = desc->skip_host_name_check ? YES : NO;

	if (desc->root_count > 0) {
		NSMutableArray *roots = [[NSMutableArray alloc] initWithCapacity:desc->root_count];
		tls->roots            = roots;
		for (uint32_t i = 0; i < desc->root_count && !CR_FAILED(result); ++i) {
			// NULL for bytes that aren't a DER certificate.
			CFDataRef der = NULL;
			if (desc->roots[i].size <= (uint64_t)LONG_MAX)
				der = CFDataCreate(NULL, (const UInt8 *)desc->roots[i].data, (CFIndex)desc->roots[i].size);
			SecCertificateRef certificate = der ? SecCertificateCreateWithData(NULL, der) : NULL;
			if (certificate)
				[roots addObject:(id)certificate];
			else
				result = CR_RESULT_DESC_INVALID_PARAMETERS;
			if (certificate)
				CFRelease(certificate);
			if (der)
				CFRelease(der);
		}
	}
	if (!CR_FAILED(result) && desc->pin_count > 0) {
		tls->pins = [[NSData alloc] initWithBytes:desc->pins length:(NSUInteger)desc->pin_count * sizeof(crnet_tls_pin)];
		if (!tls->pins)
			result = CR_RESULT_OUT_OF_MEMORY;
	}

	if (CR_FAILED(result)) {
		[tls release];
		return result;
	}
	*out_tls = tls;
	return CR_SUCCESS;
}

/* Whether the server's own certificate - the first of the chain it sent -
 * has a public key that is one of the pins. */
static BOOL crnet_apple_pin_matches(CrNetTLS *tls, SecTrustRef trust) {
	BOOL       matches = NO;
	CFArrayRef chain   = SecTrustCopyCertificateChain(trust);
	CFDataRef  der     = NULL;
	if (chain && CFArrayGetCount(chain) > 0)
		der = SecCertificateCopyData((SecCertificateRef)CFArrayGetValueAtIndex(chain, 0));

	const uint8_t *info      = NULL;
	size_t         info_size = 0;
	if (der && crnet_certificate_public_key_info(CFDataGetBytePtr(der), (size_t)CFDataGetLength(der), &info, &info_size) && info_size <= UINT32_MAX) {
		uint8_t digest[CC_SHA256_DIGEST_LENGTH];
		CC_SHA256(info, (CC_LONG)info_size, digest);
		const uint8_t *pins  = (const uint8_t *)tls->pins.bytes;
		NSUInteger     count = tls->pins.length / CRNET_TLS_PIN_SIZE;
		for (NSUInteger i = 0; i < count && !matches; ++i)
			matches = memcmp(digest, pins + i * CRNET_TLS_PIN_SIZE, CRNET_TLS_PIN_SIZE) == 0;
	}

	if (der)
		CFRelease(der);
	if (chain)
		CFRelease(chain);
	return matches;
}

/* Answers one of the session's authentication questions. Only the one about
 * the server's certificate is Carrottus Net's; the rest go back to the stack
 * unanswered. NO when the certificate was turned away - the task then ends
 * as cancelled, which says nothing of why, so the delegate keeps this and
 * `out_reason`, the record of what Security or the pins had against it. */
static BOOL crnet_apple_answer_challenge(
    CrNetTLS *tls,
    NSURLAuthenticationChallenge *challenge,
    crnet_error *out_reason,
    void (^completionHandler)(NSURLSessionAuthChallengeDisposition, NSURLCredential *)) {
	SecTrustRef trust = challenge.protectionSpace.serverTrust;
	// A desc that asks for nothing leaves the stack's own check exactly as it is.
	bool plain = tls->trust == CRNET_TLS_TRUST_SYSTEM && !tls->skip_host_name_check && !tls->pins;
	if (plain || !trust || ![challenge.protectionSpace.authenticationMethod isEqualToString:NSURLAuthenticationMethodServerTrust]) {
		completionHandler(NSURLSessionAuthChallengePerformDefaultHandling, nil);
		return YES;
	}

	BOOL accepted = YES;
	if (tls->trust != CRNET_TLS_TRUST_ANY) {
		if (tls->skip_host_name_check) {
			// The trust arrives with the policy for a TLS server of this
			// host; the same policy for no host in particular replaces it.
			SecPolicyRef policy = SecPolicyCreateSSL(true, NULL);
			accepted            = policy && SecTrustSetPolicies(trust, policy) == errSecSuccess;
			if (policy)
				CFRelease(policy);
		}
		if (accepted && tls->roots) {
			accepted = SecTrustSetAnchorCertificates(trust, (CFArrayRef)tls->roots) == errSecSuccess &&
			           SecTrustSetAnchorCertificatesOnly(trust, tls->trust == CRNET_TLS_TRUST_ROOTS) == errSecSuccess;
		}
		if (!accepted) {
			// Security wouldn't take the policy or the roots; it gives no
			// more than that it failed.
			crnet_error_record(out_reason, CR_RESULT_NET_TLS_FAILED, CR_PLATFORM_ERROR_SOURCE_OSSTATUS, errSecInternalError, NULL);
		} else {
			CFErrorRef why = NULL;
			accepted       = SecTrustEvaluateWithError(trust, &why) ? YES : NO;
			if (!accepted)
				crnet_apple_record(out_reason, CR_RESULT_NET_TLS_FAILED, (NSError *)why);
			if (why)
				CFRelease(why);
		}
	}
	if (accepted && tls->pins && !crnet_apple_pin_matches(tls, trust)) {
		crnet_error_record(out_reason, CR_RESULT_NET_TLS_FAILED, CR_PLATFORM_ERROR_SOURCE_TLS_PIN, 0, NULL);
		accepted = NO;
	}

	if (accepted)
		completionHandler(NSURLSessionAuthChallengeUseCredential, [NSURLCredential credentialForTrust:trust]);
	else
		completionHandler(NSURLSessionAuthChallengeCancelAuthenticationChallenge, nil);
	return accepted;
}

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------

/* NSURLSession moves a body by calling back, on the session's own queue,
 * and crnet.h has the caller ask for it in pieces instead. The two are
 * joined differently in each direction.
 *
 * A request body goes in through a pair of bound streams: NSURLSession
 * reads the request's body from one end as fast as the network takes it,
 * and crnet_http_transfer_send writes into the other, which has room for
 * a fixed amount and says when there is room again.
 *
 * A response body comes out one piece at a time. NSURLSession hands each
 * piece to the delegate, and the delegate does not return until the caller
 * has taken all of it - which is what holds NSURLSession back from reading
 * on ahead of a caller that isn't receiving. The cost is that the
 * session's queue stands still meanwhile, and with it every other callback
 * of that session; a client has one transfer at a time, so there is none
 * that could be kept waiting.
 *
 * Everything the callbacks write and the caller's thread reads is in one
 * object per transfer, behind one NSCondition. */

// How much request body the bound streams hold between the caller's write
// and NSURLSession's read.
#define CRNET_APPLE_BODY_STREAM_SIZE (64 * 1024)

@interface CrNetTransferState : NSObject <NSStreamDelegate> {
@public
	NSCondition *condition;

	NSURLSessionDataTask *task; // retained; nil until start
	BOOL                  follow_redirects;
	// crnet_http_transfer_abort was called. Never cleared.
	BOOL aborted;
	// The transfer is being destroyed: a delegate holding a piece for the
	// caller lets go of it.
	BOOL discarding;

	// The caller's end of the request body, or nil for a request that has
	// none to send. Its events come on `stream_queue`.
	NSOutputStream  *output; // retained
	dispatch_queue_t stream_queue;
	BOOL             output_closed;
	BOOL             output_failed;

	// The response, once its status and headers are in.
	NSHTTPURLResponse *response; // retained
	// The piece of body the delegate is holding for the caller, and how
	// much of it has been taken.
	NSData    *piece; // retained
	NSUInteger piece_offset;
	// The task's last callback has been made, and what it ended with.
	BOOL     completed;
	NSError *failure; // retained

	// A server certificate was turned away during this transfer, and why.
	BOOL          tls_refused;
	crnet_error tls_reason;
}
@end

@implementation CrNetTransferState

- (instancetype)init {
	self = [super init];
	if (self)
		condition = [[NSCondition alloc] init];
	return self;
}

- (void)dealloc {
	[condition release];
	[task release];
	[output release];
	if (stream_queue)
		dispatch_release(stream_queue);
	[response release];
	[piece release];
	[failure release];
	[super dealloc];
}

// The request body's stream has room again, or has failed. Either is
// something a send may be waiting to hear.
- (void)stream:(NSStream *)stream handleEvent:(NSStreamEvent)event {
	[condition lock];
	if (event & NSStreamEventErrorOccurred)
		output_failed = YES;
	[condition broadcast];
	[condition unlock];
}

@end

/* The session's delegate. It knows the one transfer its client has under
 * way and passes each callback on to that transfer's state - or drops it,
 * when it is for a task that has since been destroyed and replaced. */
@interface CrNetHTTPDelegate : NSObject <NSURLSessionDataDelegate> {
@public
	// The client's, for as long as the client lasts.
	CrNetTLS *tls; // retained
	// Guards `current`.
	NSLock               *lock;
	CrNetTransferState *current; // retained, or nil
}
@end

@implementation CrNetHTTPDelegate

- (instancetype)init {
	self = [super init];
	if (self)
		lock = [[NSLock alloc] init];
	return self;
}

- (void)dealloc {
	[tls release];
	[lock release];
	[current release];
	[super dealloc];
}

// The state `task` belongs to, kept alive for the callback that asked, or
// nil for a task that is no longer the client's transfer.
- (CrNetTransferState *)stateForTask:(NSURLSessionTask *)task {
	[lock lock];
	CrNetTransferState *state = (current && current->task == task) ? [[current retain] autorelease] : nil;
	[lock unlock];
	return state;
}

- (void)URLSession:(NSURLSession *)session
    didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
      completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
	// A challenge comes for a connection, not for a task, so it is the
	// transfer under way that it is answered for.
	[lock lock];
	CrNetTransferState *state = [[current retain] autorelease];
	[lock unlock];

	crnet_error reason;
	crnet_error_clear(&reason);
	if (!crnet_apple_answer_challenge(tls, challenge, &reason, completionHandler) && state) {
		[state->condition lock];
		state->tls_refused = YES;
		state->tls_reason  = reason;
		[state->condition unlock];
	}
}

- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)redirection
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
	// nil ends the task with the 3xx itself as its response.
	CrNetTransferState *state = [self stateForTask:task];
	completionHandler((state && state->follow_redirects) ? request : nil);
}

- (void)URLSession:(NSURLSession *)session
                 task:(NSURLSessionTask *)task
    needNewBodyStream:(void (^)(NSInputStream *))completionHandler {
	// NSURLSession wants the request body from its beginning again. It has
	// passed through and is not here to give.
	completionHandler(nil);
}

- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveResponse:(NSURLResponse *)answer
     completionHandler:(void (^)(NSURLSessionResponseDisposition))completionHandler {
	CrNetTransferState *state = [self stateForTask:dataTask];
	if (!state) {
		completionHandler(NSURLSessionResponseCancel);
		return;
	}
	[state->condition lock];
	if ([answer isKindOfClass:[NSHTTPURLResponse class]]) {
		[state->response release];
		state->response = (NSHTTPURLResponse *)[answer retain];
	}
	[state->condition broadcast];
	[state->condition unlock];
	completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
	CrNetTransferState *state = [self stateForTask:dataTask];
	if (!state || data.length == 0)
		return;

	// NSURLSession hands the body over already decoded. This doesn't
	// return until the caller has taken the whole piece, or has shown it
	// never will.
	[state->condition lock];
	if (!state->aborted && !state->discarding) {
		state->piece        = [data retain];
		state->piece_offset = 0;
		[state->condition broadcast];
		while (state->piece && !state->aborted && !state->discarding)
			[state->condition wait];
		[state->piece release];
		state->piece = nil;
	}
	[state->condition unlock];
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
	// Always the task's last callback, cancelled or not.
	CrNetTransferState *state = [self stateForTask:task];
	if (!state)
		return;
	[state->condition lock];
	state->completed = YES;
	state->failure   = [error retain];
	[state->condition broadcast];
	[state->condition unlock];
}

@end

struct crnet_http_client_t {
	cr_object_head     head; // first: crnethttp.c reaches the allocator through it
	NSURLSession        *session;
	CrNetHTTPDelegate *delegate;
};

/* The backend's side of a crnet_http_transfer_t. */
struct crnet_apple_http {
	struct crnet_http_client_t *client;
	CrNetTransferState         *state;
};

CR_RESULT crnet_http_client_create(
    crnet_instance instance,
    const crnet_http_client_desc *desc,
    crnet_http_client *out_client) {
	if (!out_client)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_client = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_http_client_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_HTTP_CLIENT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;

	CrNetTLS *tls    = nil;
	CR_RESULT  result = crnet_apple_tls_create(&desc->tls, &tls);
	if (CR_FAILED(result))
		return result;
	const CR_ALLOCATOR *allocator = &instance->head.allocator;
	struct crnet_proxy  proxy;
	result = crnet_proxy_parse(allocator, &desc->proxy, &proxy);
	if (CR_FAILED(result)) {
		crnet_proxy_free(allocator, &proxy);
		[tls release];
		return result;
	}

	struct crnet_http_client_t *client = CR_NEW_ZEROED(allocator, struct crnet_http_client_t);
	if (!client) {
		crnet_proxy_free(allocator, &proxy);
		[tls release];
		return CR_RESULT_OUT_OF_MEMORY;
	}
	client->head.allocator = *allocator;
	CR_BIRTH_IN(client, &instance->quarantine);

	@autoreleasepool {
		NSURLSessionConfiguration *configuration = crnet_apple_configuration(&proxy);
		client->delegate                         = [[CrNetHTTPDelegate alloc] init];
		// The delegate's from here, or nobody's.
		if (client->delegate)
			client->delegate->tls = tls;
		else
			[tls release];
		// A serial queue of the session's own making carries the callbacks.
		if (configuration && client->delegate)
			client->session = [[NSURLSession sessionWithConfiguration:configuration delegate:client->delegate delegateQueue:nil] retain];
	}
	crnet_proxy_free(allocator, &proxy);
	if (!client->delegate || !client->session) {
		[client->session invalidateAndCancel];
		[client->session release];
		[client->delegate release];
		CR_DELETE(allocator, struct crnet_http_client_t, client);
		return CR_RESULT_GENERAL_FAILURE;
	}

	*out_client = client;
	return CR_SUCCESS;
}

void crnet_http_client_destroy(crnet_http_client client) {
	if (!client)
		return;
	if (CR_STALE(client, "crnet_http_client_destroy", "client"))
		return;
	// The session holds its delegate until it is invalidated.
	[client->session invalidateAndCancel];
	[client->session release];
	[client->delegate release];
	CR_KILL(client);
	const CR_ALLOCATOR allocator = client->head.allocator;
	cr_string_free(&allocator, client->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(client), &allocator, struct crnet_http_client_t, client);
}

/* The record and the result for a transfer NSURLSession ended with
 * `error`. A certificate the delegate turned away ends the task as
 * cancelled, which says nothing of why, so that is told apart by what was
 * done. Called with the state's condition held. */
static CR_RESULT crnet_apple_http_failed(struct crnet_http_transfer_t *transfer, CrNetTransferState *state, NSError *error, CR_RESULT otherwise) {
	if (state->tls_refused) {
		transfer->error = state->tls_reason;
		return CR_RESULT_NET_TLS_FAILED;
	}
	return crnet_apple_record(&transfer->error, crnet_apple_result(error, otherwise), error);
}

CR_RESULT crnet_http_backend_create(struct crnet_http_transfer_t *transfer) {
	struct crnet_apple_http *http = CR_NEW_ZEROED(&transfer->head.allocator, struct crnet_apple_http);
	if (!http)
		return CR_RESULT_OUT_OF_MEMORY;
	http->client = transfer->client;
	http->state  = [[CrNetTransferState alloc] init];
	if (!http->state || !http->state->condition) {
		[http->state release];
		CR_DELETE(&transfer->head.allocator, struct crnet_apple_http, http);
		return CR_RESULT_OUT_OF_MEMORY;
	}
	transfer->backend = http;
	return CR_SUCCESS;
}

void crnet_http_backend_destroy(struct crnet_http_transfer_t *transfer) {
	struct crnet_apple_http *http = (struct crnet_apple_http *)transfer->backend;
	if (!http)
		return;
	CrNetTransferState *state = http->state;

	// A delegate holding a piece for the caller is let go first: it is
	// standing on the queue everything else here has to get through.
	[state->condition lock];
	state->discarding = YES;
	BOOL completed    = state->completed;
	[state->condition broadcast];
	[state->condition unlock];

	// A task that ran to its end has left its connection with the session.
	// Cancelling one that didn't is what drops its connection.
	if (!completed)
		[state->task cancel];

	if (state->output) {
		state->output.delegate = nil;
		CFWriteStreamSetDispatchQueue((CFWriteStreamRef)state->output, NULL);
		[state->output close];
	}

	// From here the delegate passes nothing more on to this state; a
	// callback already on its way holds the state itself until it is done.
	CrNetHTTPDelegate *delegate = http->client->delegate;
	[delegate->lock lock];
	if (delegate->current == state) {
		[delegate->current release];
		delegate->current = nil;
	}
	[delegate->lock unlock];

	[state release];
	CR_DELETE(&transfer->head.allocator, struct crnet_apple_http, http);
	transfer->backend = NULL;
}

void crnet_http_backend_abort(struct crnet_http_transfer_t *transfer) {
	CrNetTransferState *state = ((struct crnet_apple_http *)transfer->backend)->state;
	[state->condition lock];
	state->aborted = YES;
	// Cancelling ends the task, which wakes whoever was waiting on it, and
	// the broadcast wakes a delegate that was holding a piece.
	[state->task cancel];
	[state->condition broadcast];
	[state->condition unlock];
}

bool crnet_http_backend_aborted(struct crnet_http_transfer_t *transfer) {
	CrNetTransferState *state = ((struct crnet_apple_http *)transfer->backend)->state;
	[state->condition lock];
	bool aborted = state->aborted;
	[state->condition unlock];
	return aborted;
}

CR_RESULT crnet_http_backend_start(
    struct crnet_http_transfer_t *transfer,
    const crnet_http_transfer_desc *desc,
    enum crnet_accept_encoding encoding) {
	// NSURLSession sends a caller's Accept-Encoding in place of its own and
	// needs no telling.
	(void)encoding;
	struct crnet_apple_http *http     = (struct crnet_apple_http *)transfer->backend;
	CrNetTransferState      *state    = http->state;
	CrNetHTTPDelegate       *delegate = http->client->delegate;

	@autoreleasepool {
		NSMutableURLRequest *request        = nil;
		CR_RESULT           request_result = crnet_apple_request(desc->url, desc->headers, desc->header_count, &request);
		if (CR_FAILED(request_result))
			return request_result;
		NSString *method = [NSString stringWithUTF8String:desc->method];
		if (!method)
			return CR_RESULT_DESC_INVALID_PARAMETERS;
		// The method as given. NSURLSession itself knows a HEAD's response
		// has no body, by the same exact name crnet.h goes by.
		request.HTTPMethod = method;

		// What the request says of its body comes from the desc. A sized
		// body is announced here; one that is there and empty is given as
		// just that; one with bytes to come, sized or not, is read by
		// NSURLSession from the stream the caller's sends write into.
		BOOL streams = NO;
		if (transfer->body == CRNET_HTTP_BODY_SIZED) {
			[request setValue:[NSString stringWithFormat:@"%llu", (unsigned long long)transfer->body_size] forHTTPHeaderField:@"Content-Length"];
			if (transfer->body_size == 0)
				request.HTTPBody = [NSData data];
			else
				streams = YES;
		} else if (transfer->body == CRNET_HTTP_BODY_UNSIZED) {
			streams = YES;
		}
		if (streams) {
			NSInputStream  *input  = nil;
			NSOutputStream *output = nil;
			[NSStream getBoundStreamsWithBufferSize:CRNET_APPLE_BODY_STREAM_SIZE inputStream:&input outputStream:&output];
			if (!input || !output)
				return CR_RESULT_OUT_OF_MEMORY;
			request.HTTPBodyStream = input;
			state->output          = [output retain];
			state->stream_queue    = dispatch_queue_create("crnet.http.body", DISPATCH_QUEUE_SERIAL);
			// The stream's events - room again, or an error - come on a
			// queue of its own, not on a run loop the caller would have to
			// be running.
			output.delegate = state;
			CFWriteStreamSetDispatchQueue((CFWriteStreamRef)output, state->stream_queue);
			[output open];
		}

		// One transfer on a client at a time: the delegate passes its
		// callbacks to one state.
		[delegate->lock lock];
		if (delegate->current) {
			[delegate->lock unlock];
			return CR_RESULT_NET_INVALID_HTTP_TRANSFER;
		}
		// Under both locks, so that an abort lands either before the task
		// exists (and the first wait sees it) or on a task it can cancel.
		[state->condition lock];
		state->follow_redirects = desc->follow_redirects ? YES : NO;
		state->task             = [[http->client->session dataTaskWithRequest:request] retain];
		BOOL made               = state->task != nil;
		BOOL aborted            = state->aborted;
		if (made)
			delegate->current = [state retain];
		[state->condition unlock];
		[delegate->lock unlock];
		if (!made)
			return CR_RESULT_NET_HTTP_FAILED;
		if (aborted)
			return CR_RESULT_NET_ABORTED;

		// NSURLSession connects when it is ready to and says nothing of it,
		// so there is nothing here to wait for: a server that can't be
		// reached is reported by the first call that needs it, as crnet.h
		// allows.
		[state->task resume];
	}
	return CR_SUCCESS;
}

CR_RESULT crnet_http_backend_send(
    struct crnet_http_transfer_t *transfer,
    const void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_sent) {
	CrNetTransferState *state = ((struct crnet_apple_http *)transfer->backend)->state;

	@autoreleasepool {
		NSDate    *limit  = crnet_apple_date_limit(timeout_ms);
		CR_RESULT result = CR_SUCCESS;

		[state->condition lock];
		// With CRNET_NO_WAIT the limit has already passed and this only
		// looks at whether there is room.
		while (!state->aborted && !state->completed && !state->response && !state->output_failed && !state->output.hasSpaceAvailable) {
			if (![state->condition waitUntilDate:limit])
				break;
		}

		if (state->aborted) {
			result = CR_RESULT_NET_ABORTED;
		} else if (state->completed && state->failure) {
			// The task ended while the body was still going in.
			result = crnet_apple_http_failed(transfer, state, state->failure, CR_RESULT_NET_SEND_FAILED);
		} else if (state->completed || state->response) {
			// The server has answered without waiting for the rest of the
			// body - a refusal, usually. NSURLSession stops reading the
			// body once it has the answer, so the stream would never have
			// room again; there is nowhere for the bytes to go and a
			// response for the caller to read, and they are let through as
			// sent.
			*out_sent = size;
		} else if (state->output_failed) {
			result = crnet_apple_http_failed(transfer, state, state->output.streamError, CR_RESULT_NET_SEND_FAILED);
		} else if (!state->output.hasSpaceAvailable) {
			result = CR_RESULT_NET_TIMED_OUT;
		} else {
			// With room there, this takes what fits and returns.
			NSInteger written = [state->output write:(const uint8_t *)data maxLength:(NSUInteger)size];
			if (written > 0)
				*out_sent = (size_t)written;
			else
				result = crnet_apple_http_failed(transfer, state, state->output.streamError, CR_RESULT_NET_SEND_FAILED);
		}
		[state->condition unlock];
		return result;
	}
}

CR_RESULT crnet_http_backend_receive_response(
    struct crnet_http_transfer_t *transfer,
    uint32_t timeout_ms) {
	CrNetTransferState *state = ((struct crnet_apple_http *)transfer->backend)->state;

	@autoreleasepool {
		NSDate *limit = crnet_apple_date_limit(timeout_ms);

		[state->condition lock];
		// Said once: closing the caller's end is how NSURLSession, reading
		// the other, comes to the body's end.
		if (state->output && !state->output_closed) {
			state->output_closed = YES;
			[state->output close];
		}
		while (!state->aborted && !state->completed && !state->response) {
			if (![state->condition waitUntilDate:limit])
				break;
		}

		CR_RESULT result = CR_SUCCESS;
		if (state->aborted) {
			result = CR_RESULT_NET_ABORTED;
		} else if (state->response) {
			// NSURLSession gives the headers with a repeated name already
			// joined into one value, which is the form crnet.h promises.
			transfer->response.status = (uint32_t)state->response.statusCode;
			__block bool stored       = true;
			[state->response.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id object, BOOL *stop) {
				if (![key isKindOfClass:[NSString class]] || ![object isKindOfClass:[NSString class]])
					return;
				const char *name  = [(NSString *)key UTF8String];
				const char *value = [(NSString *)object UTF8String];
				if (!name || !value)
					return;
				// On the caller's thread: enumerating runs the block here.
				if (!crnet_head_add_header(&transfer->head.allocator, &transfer->response, name, strlen(name), value, strlen(value))) {
					stored = false;
					*stop  = YES;
				}
			}];
			if (!stored)
				result = CR_RESULT_OUT_OF_MEMORY;
		} else if (state->completed) {
			// Ended with no response.
			result = crnet_apple_http_failed(transfer, state, state->failure, CR_RESULT_NET_HTTP_FAILED);
		} else {
			result = CR_RESULT_NET_TIMED_OUT;
		}
		[state->condition unlock];
		return result;
	}
}

CR_RESULT crnet_http_backend_recv(
    struct crnet_http_transfer_t *transfer,
    void *data,
    size_t size,
    uint32_t timeout_ms,
    size_t *out_received) {
	CrNetTransferState *state = ((struct crnet_apple_http *)transfer->backend)->state;

	@autoreleasepool {
		NSDate *limit = crnet_apple_date_limit(timeout_ms);

		[state->condition lock];
		// With CRNET_NO_WAIT the limit has already passed and this only
		// looks at what is there.
		while (!state->aborted && !state->completed && !state->piece) {
			if (![state->condition waitUntilDate:limit])
				break;
		}

		CR_RESULT result = CR_SUCCESS;
		if (state->aborted) {
			result = CR_RESULT_NET_ABORTED;
		} else if (state->piece) {
			NSUInteger left  = state->piece.length - state->piece_offset;
			NSUInteger bytes = size < (size_t)left ? (NSUInteger)size : left;
			// A piece need not be one stretch of memory; this copies across
			// whatever it is made of.
			[state->piece getBytes:data range:NSMakeRange(state->piece_offset, bytes)];
			state->piece_offset += bytes;
			*out_received        = (size_t)bytes;
			if (state->piece_offset == state->piece.length) {
				// All taken: the delegate holding it can go back for more.
				[state->piece release];
				state->piece = nil;
				[state->condition broadcast];
			}
		} else if (state->completed) {
			if (state->failure)
				result = crnet_apple_http_failed(transfer, state, state->failure, CR_RESULT_NET_RECV_FAILED);
			else
				*out_received = 0; // the body's end
		} else {
			result = CR_RESULT_NET_TIMED_OUT;
		}
		[state->condition unlock];
		return result;
	}
}

#if CRNET_EXT_WEBSOCKET

// --------------------------------------------------------------------------
// WebSocket
// --------------------------------------------------------------------------

/* Everything the session's callbacks write and the caller's threads read,
 * behind one NSCondition. It is the session's delegate and is also retained
 * by each receive block, so it outlives a socket that was destroyed with a
 * callback still on its way. */
@interface CrNetWebSocketState : NSObject <NSURLSessionWebSocketDelegate> {
@public
	NSCondition *condition;

	// crnet_websocket_abort was called. Never cleared.
	BOOL aborted;

	// The opening request: 0 not started or still waiting, 1 open, 2 failed.
	BOOL      connect_called;
	int       open_state;
	uint32_t  http_status;
	NSError  *open_error;
	// Set by connect before the session exists. A server certificate it
	// turned away is recorded for connect to report.
	CrNetTLS   *tls;
	BOOL          tls_refused;
	crnet_error tls_reason;
	// What the receive that ended the connection failed with.
	crnet_error receive_error;

	// The wait set this socket is in, or NULL. Told, with the condition
	// held, whenever what a receive would find has changed.
	struct crnet_wait_set_t *wait_set;

	// From the connect desc: what becomes of an opening request the server
	// answers with a redirect.
	BOOL follow_redirects;

	// A receive has been asked of the task and hasn't called back yet.
	BOOL receive_pending;
	// The message being handed to the caller, and how much of it has gone.
	BOOL                      has_message;
	NSData                   *message;
	NSUInteger                message_offset;
	CRNET_WEBSOCKET_MESSAGE message_type;

	// Nothing more will arrive: a receive failed, or a send ran out of time.
	BOOL finished;
	BOOL too_big;
	// The peer's close frame arrived / this side sent one.
	BOOL     peer_closed;
	uint16_t close_code;
	BOOL     close_requested;
}
@end

@implementation CrNetWebSocketState

- (instancetype)init {
	self = [super init];
	if (self)
		condition = [[NSCondition alloc] init];
	return self;
}

- (void)dealloc {
	[condition release];
	[open_error release];
	[message release];
	[tls release];
	[super dealloc];
}

- (void)URLSession:(NSURLSession *)session
    didReceiveChallenge:(NSURLAuthenticationChallenge *)challenge
      completionHandler:(void (^)(NSURLSessionAuthChallengeDisposition, NSURLCredential *))completionHandler {
	crnet_error reason;
	crnet_error_clear(&reason);
	if (!crnet_apple_answer_challenge(tls, challenge, &reason, completionHandler)) {
		[condition lock];
		tls_refused = YES;
		tls_reason  = reason;
		[condition unlock];
	}
}

- (void)URLSession:(NSURLSession *)session
          webSocketTask:(NSURLSessionWebSocketTask *)webSocketTask
    didOpenWithProtocol:(NSString *)protocol {
	[condition lock];
	if (open_state == 0) {
		open_state  = 1;
		http_status = 101;
	}
	[condition broadcast];
	[condition unlock];
}

- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)redirection
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
	// Answered either way, so that it is the desc and not NSURLSession's
	// default that decides. nil ends the task with the 3xx as its
	// response, which connect then reports as the server's refusal.
	completionHandler(follow_redirects ? request : nil);
}

- (void)URLSession:(NSURLSession *)session
       webSocketTask:(NSURLSessionWebSocketTask *)webSocketTask
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)closeCode
              reason:(NSData *)reason {
	// Only recorded: messages that arrived ahead of the close frame are
	// still handed over, and the receive that follows them is what fails.
	[condition lock];
	peer_closed = YES;
	close_code  = (uint16_t)closeCode;
	[condition broadcast];
	[condition unlock];
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
	[condition lock];
	if (open_state == 0) {
		open_state = 2;
		open_error = [error retain];
		if ([task.response isKindOfClass:[NSHTTPURLResponse class]])
			http_status = (uint32_t)((NSHTTPURLResponse *)task.response).statusCode;
	}
	[condition broadcast];
	[condition unlock];
}

@end

struct crnet_websocket_t {
	cr_object_head       head;
	crnet_instance       instance;
	CrNetWebSocketState *state;
	// Made by connect, under the state's condition; nil until then.
	NSURLSession              *session;
	NSURLSessionWebSocketTask *task;

	// By CRNET_ERROR_SIDE; each is written only by its side's caller.
	crnet_error errors[CRNET_ERROR_SIDE_COUNT];
};

crnet_instance crnet_websocket_instance(crnet_websocket socket) { return socket ? socket->instance : NULL; }

CR_RESULT crnet_websocket_create(
    crnet_instance instance,
    crnet_websocket *out_socket) {
	if (!out_socket)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_socket = NULL;
	if (!instance)
		return CR_RESULT_NET_INVALID_INSTANCE;
	if (CR_STALE(instance, "crnet_websocket_create", "instance"))
		return CR_RESULT_NET_INVALID_INSTANCE;

	struct crnet_websocket_t *socket = CR_NEW_ZEROED(&instance->head.allocator, struct crnet_websocket_t);
	if (!socket)
		return CR_RESULT_OUT_OF_MEMORY;
	socket->head.allocator = instance->head.allocator;
	CR_BIRTH_IN(socket, &instance->quarantine);
	socket->instance       = instance;
	socket->state          = [[CrNetWebSocketState alloc] init];
	if (!socket->state || !socket->state->condition) {
		[socket->state release];
		CR_DELETE(&instance->head.allocator, struct crnet_websocket_t, socket);
		return CR_RESULT_OUT_OF_MEMORY;
	}

	*out_socket = socket;
	return CR_SUCCESS;
}

void crnet_websocket_destroy(crnet_websocket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_websocket_destroy", "socket"))
		return;
	// The state outlives the socket while a callback is on its way; it must
	// not outlive the socket's place in a wait set.
	[socket->state->condition lock];
	socket->state->wait_set = NULL;
	[socket->state->condition unlock];
	[socket->task cancel];
	[socket->session invalidateAndCancel];
	[socket->task release];
	[socket->session release];
	[socket->state release];
	CR_KILL(socket);
	const CR_ALLOCATOR allocator = socket->head.allocator;
	cr_string_free(&allocator, socket->head.debug_name);
	CR_RETIRE(CR_QUARANTINE_OF(socket), &allocator, struct crnet_websocket_t, socket);
}

void crnet_websocket_get_error(
    crnet_websocket socket,
    CRNET_ERROR_SIDE side,
    crnet_error *out_error) {
	if (!socket) {
		crnet_error_get(NULL, out_error);
		return;
	}
	if (CR_STALE(socket, "crnet_websocket_get_error", "socket")) {
		crnet_error_get(NULL, out_error);
		return;
	}
	bool known = side == CRNET_ERROR_SIDE_RECEIVING || side == CRNET_ERROR_SIDE_SENDING;
	crnet_error_get(known ? &socket->errors[side] : NULL, out_error);
}

void crnet_websocket_abort(crnet_websocket socket) {
	if (!socket)
		return;
	if (CR_STALE(socket, "crnet_websocket_abort", "socket"))
		return;
	CrNetWebSocketState *state = socket->state;
	[state->condition lock];
	state->aborted = YES;
	// Cancelling fails whatever the task has outstanding - the opening
	// request, a receive, a send - and each of those wakes its waiter.
	[socket->task cancel];
	[state->condition broadcast];
	crnet_wait_set_notify(state->wait_set);
	[state->condition unlock];
}

CR_RESULT crnet_websocket_connect(
    crnet_websocket socket,
    const crnet_websocket_connect_desc *desc,
    uint32_t *out_http_status) {
	if (out_http_status)
		*out_http_status = 0;
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_connect", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (!desc)
		return CR_RESULT_NULL_DESC;
	const CR_RESULT base_sr = cr_struct_check(&desc->base, CR_STRUCT_TYPE_NET_WEBSOCKET_CONNECT_DESC);
	if (base_sr != CR_SUCCESS)
		return base_sr;
	if (!desc->url || desc->max_message_size == 0 || (desc->header_count > 0 && !desc->headers))
		return CR_RESULT_DESC_INVALID_PARAMETERS;
	if (!crnet_url_has_scheme(desc->url, "ws") && !crnet_url_has_scheme(desc->url, "wss"))
		return CR_RESULT_NET_INVALID_URL;

	@autoreleasepool {
		NSMutableURLRequest *request        = nil;
		CR_RESULT           request_result = crnet_apple_request(desc->url, desc->headers, desc->header_count, &request);
		if (CR_FAILED(request_result))
			return request_result;

		CrNetTLS *tls        = nil;
		CR_RESULT  tls_result = crnet_apple_tls_create(&desc->tls, &tls);
		if (CR_FAILED(tls_result))
			return tls_result;
		// The proxy is only needed for as long as it takes to put it in a
		// configuration, which is made here, ahead of the lock.
		struct crnet_proxy proxy;
		CR_RESULT           proxy_result = crnet_proxy_parse(&socket->head.allocator, &desc->proxy, &proxy);
		NSURLSessionConfiguration *configuration = CR_FAILED(proxy_result) ? nil : crnet_apple_configuration(&proxy);
		crnet_proxy_free(&socket->head.allocator, &proxy);
		if (!configuration) {
			[tls release];
			return CR_FAILED(proxy_result) ? proxy_result : CR_RESULT_DESC_INVALID_PARAMETERS;
		}

		CrNetWebSocketState *state = socket->state;
		[state->condition lock];
		if (state->aborted) {
			[state->condition unlock];
			[tls release];
			return CR_RESULT_NET_ABORTED;
		}
		if (state->connect_called) {
			[state->condition unlock];
			[tls release];
			return CR_RESULT_NET_INVALID_WEBSOCKET;
		}
		if (desc->timeout_ms == CRNET_NO_WAIT) {
			[state->condition unlock];
			[tls release];
			return CR_RESULT_NET_TIMED_OUT;
		}
		state->connect_called   = YES;
		state->tls              = tls; // the state's from here
		state->follow_redirects = desc->follow_redirects ? YES : NO;

		// Made under the condition, so an abort lands either before this
		// (and was seen above) or on a task it can cancel.
		socket->session = [[NSURLSession sessionWithConfiguration:configuration delegate:state delegateQueue:nil] retain];
		socket->task    = [[socket->session webSocketTaskWithRequest:request] retain];
		if (!socket->session || !socket->task) {
			state->open_state = 2;
			[state->condition unlock];
			return CR_RESULT_GENERAL_FAILURE;
		}
		socket->task.maximumMessageSize = desc->max_message_size > (uint64_t)NSIntegerMax ? NSIntegerMax : (NSInteger)desc->max_message_size;
		[socket->task resume];

		NSDate *limit = crnet_apple_date_limit(desc->timeout_ms);
		while (state->open_state == 0 && !state->aborted) {
			if (![state->condition waitUntilDate:limit])
				break;
		}
		BOOL      aborted    = state->aborted;
		int       open_state = state->open_state;
		uint32_t  status     = state->http_status;
		NSError  *error      = [[state->open_error retain] autorelease];
		BOOL          refused = state->tls_refused;
		crnet_error reason  = state->tls_reason;
		if (aborted || open_state != 1) {
			// Out of time, refused or aborted: this socket is spent.
			state->open_state = 2;
			state->finished   = YES;
			[socket->task cancel];
		}
		[state->condition unlock];

		if (out_http_status)
			*out_http_status = status;
		if (aborted)
			return CR_RESULT_NET_ABORTED;
		if (open_state == 1)
			return CR_SUCCESS;
		if (open_state == 0)
			return CR_RESULT_NET_TIMED_OUT;
		crnet_error *record = &socket->errors[CRNET_ERROR_SIDE_RECEIVING];
		if (refused) {
			*record = reason;
			return CR_RESULT_NET_TLS_FAILED;
		}
		if (status != 0 && status != 101)
			return CR_RESULT_NET_WS_UPGRADE_REFUSED;
		return crnet_apple_record(record, crnet_apple_result(error, CR_RESULT_NET_CONNECT_FAILED), error);
	}
}

/* What a call on a socket that can't carry it returns, or CR_SUCCESS for
 * one that can. Called with the condition held. */
static CR_RESULT crnet_apple_websocket_usable(CrNetWebSocketState *state) {
	if (state->aborted)
		return CR_RESULT_NET_ABORTED;
	if (state->open_state != 1)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	return CR_SUCCESS;
}

CR_RESULT crnet_websocket_send(
    crnet_websocket socket,
    CRNET_WEBSOCKET_MESSAGE type,
    const void *data,
    uint64_t size,
    uint32_t timeout_ms) {
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_send", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if ((size > 0 && !data) || size > NSUIntegerMax)
		return CR_RESULT_NET_SEND_FAILED;

	@autoreleasepool {
		CrNetWebSocketState *state = socket->state;
		[state->condition lock];
		CR_RESULT usable = crnet_apple_websocket_usable(state);
		if (!CR_FAILED(usable) && state->finished)
			usable = CR_RESULT_NET_SEND_FAILED;
		[state->condition unlock];
		if (CR_FAILED(usable))
			return usable;
		if (timeout_ms == CRNET_NO_WAIT)
			return CR_RESULT_NET_TIMED_OUT;

		NSURLSessionWebSocketMessage *message = nil;
		if (type == CRNET_WEBSOCKET_MESSAGE_TEXT) {
			// nil when the bytes aren't UTF-8, which a text message must be.
			NSString *text = [[[NSString alloc] initWithBytes:(size > 0 ? data : "") length:(NSUInteger)size encoding:NSUTF8StringEncoding] autorelease];
			if (!text)
				return CR_RESULT_NET_SEND_FAILED;
			message = [[[NSURLSessionWebSocketMessage alloc] initWithString:text] autorelease];
		} else {
			NSData *bytes = [NSData dataWithBytes:(size > 0 ? data : "") length:(NSUInteger)size];
			message       = [[[NSURLSessionWebSocketMessage alloc] initWithData:bytes] autorelease];
		}
		if (!message)
			return CR_RESULT_OUT_OF_MEMORY;

		// `failed` moves to the heap with the block, so a callback that
		// arrives after this has given up waiting still has somewhere to
		// write. An abort cancels the task, which fails the send and so
		// ends the wait.
		dispatch_semaphore_t  done   = dispatch_semaphore_create(0);
		__block BOOL          failed = NO;
		__block crnet_error failure;
		[socket->task sendMessage:message
		        completionHandler:^(NSError *error) {
			        failed = error != nil;
			        if (failed)
				        crnet_apple_record(&failure, CR_RESULT_NET_SEND_FAILED, error);
			        dispatch_semaphore_signal(done);
		        }];
		bool timed_out = dispatch_semaphore_wait(done, crnet_apple_dispatch_limit(timeout_ms)) != 0;
		dispatch_release(done);

		[state->condition lock];
		BOOL aborted = state->aborted;
		if (timed_out && !aborted) {
			// Part of the message may be on the wire; the connection can't
			// be trusted with another.
			state->finished = YES;
			[socket->task cancel];
			[state->condition broadcast];
			crnet_wait_set_notify(state->wait_set);
		}
		[state->condition unlock];

		if (aborted)
			return CR_RESULT_NET_ABORTED;
		if (timed_out)
			return CR_RESULT_NET_TIMED_OUT;
		if (!failed)
			return CR_SUCCESS;
		// The block has run, so its record is this thread's to read.
		socket->errors[CRNET_ERROR_SIDE_SENDING] = failure;
		return CR_RESULT_NET_SEND_FAILED;
	}
}

/* Whether a failed receive failed because the message was over the task's
 * maximumMessageSize. NSURLSession has no error of its own for that; what
 * it reports is the POSIX "message too long" from the layer under it, and
 * like any error from down there it may come wrapped in another, so the
 * errors underneath are looked at too. */
static BOOL crnet_apple_is_message_too_long(NSError *error) {
	// No real chain is this deep; the bound is against one that loops.
	for (int depth = 0; error && depth < 8; ++depth) {
		if ([error.domain isEqualToString:NSPOSIXErrorDomain] && error.code == EMSGSIZE)
			return YES;
		id underneath = error.userInfo[NSUnderlyingErrorKey];
		error         = [underneath isKindOfClass:[NSError class]] ? (NSError *)underneath : nil;
	}
	return NO;
}

/* Asks the task for its next message. Called with the condition held; the
 * answer comes later on the session's queue. NSURLSession hands over whole
 * messages only, so one answer is one message. */
static void crnet_apple_websocket_start_receive(struct crnet_websocket_t *socket) {
	CrNetWebSocketState *state = socket->state;
	state->receive_pending       = YES;
	[socket->task receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *received, NSError *error) {
		[state->condition lock];
		state->receive_pending = NO;
		// After this side's close nothing more is handed over, a message
		// that was already on its way included.
		if (error || !received || state->close_requested) {
			state->finished = YES;
			state->too_big  = crnet_apple_is_message_too_long(error);
			// Kept whatever the ending turns out to be called; recv sets
			// the result it reports.
			crnet_apple_record(&state->receive_error, CR_RESULT_NET_RECV_FAILED, error);
		} else {
			if (received.type == NSURLSessionWebSocketMessageTypeString) {
				state->message      = [[received.string dataUsingEncoding:NSUTF8StringEncoding] retain];
				state->message_type = CRNET_WEBSOCKET_MESSAGE_TEXT;
			} else {
				state->message      = [received.data retain];
				state->message_type = CRNET_WEBSOCKET_MESSAGE_BINARY;
			}
			state->message_offset = 0;
			state->has_message    = YES;
		}
		[state->condition broadcast];
		crnet_wait_set_notify(state->wait_set);
		[state->condition unlock];
	}];
}

// For a wait set (see the end of crnetshared.h). NSURLSession reports
// through callbacks, so there is no socket to poll: the socket keeps its
// set, tells it above whenever a receive has an answer, and is asked here.

CR_RESULT crnet_websocket_wait_attach(
    crnet_websocket socket,
    struct crnet_wait_set_t *set,
    bool *out_has_native,
    intptr_t *out_native,
    bool *out_report_once) {
	CrNetWebSocketState *state = socket->state;
	[state->condition lock];
	CR_RESULT result = CR_SUCCESS;
	if (state->aborted || state->open_state != 1)
		result = CR_RESULT_NET_INVALID_WEBSOCKET;
	else if (state->wait_set)
		result = CR_RESULT_NET_ALREADY_IN_WAIT_SET;
	if (!CR_FAILED(result)) {
		state->wait_set = set;
		// NSURLSession only says a message has arrived to a receive that
		// was asked for; without one the set would never hear of it.
		if (!state->has_message && !state->finished && !state->receive_pending)
			crnet_apple_websocket_start_receive(socket);
	}
	[state->condition unlock];
	if (CR_FAILED(result))
		return result;

	*out_has_native  = false;
	*out_native      = 0;
	*out_report_once = false;
	return CR_SUCCESS;
}

void crnet_websocket_wait_detach(crnet_websocket socket) {
	CrNetWebSocketState *state = socket->state;
	// Every notification is made with the condition held, so none is under
	// way once this has it.
	[state->condition lock];
	state->wait_set = NULL;
	[state->condition unlock];
}

bool crnet_websocket_wait_ready(crnet_websocket socket) {
	CrNetWebSocketState *state = socket->state;
	[state->condition lock];
	bool ready = state->aborted || state->has_message || state->finished;
	[state->condition unlock];
	return ready;
}

CR_RESULT crnet_websocket_recv(
    crnet_websocket socket,
    void *data,
    uint64_t size,
    uint32_t timeout_ms,
    uint64_t *out_received,
    CRNET_WEBSOCKET_MESSAGE *out_type,
    bool *out_message_complete) {
	if (!out_received || !out_type || !out_message_complete)
		return CR_RESULT_NULL_OUTPUT_PARAMETER;
	*out_received         = 0;
	*out_type             = CRNET_WEBSOCKET_MESSAGE_BINARY;
	*out_message_complete = false;
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_recv", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (!data || size == 0)
		return CR_RESULT_ZERO_SIZE;

	@autoreleasepool {
		CrNetWebSocketState *state = socket->state;
		NSDate                *limit = crnet_apple_date_limit(timeout_ms);

		[state->condition lock];
		CR_RESULT result = crnet_apple_websocket_usable(state);
		if (CR_FAILED(result)) {
			[state->condition unlock];
			return result;
		}

		// A receive left waiting by a call that timed out is still the one
		// that will answer; a second would take the message after it.
		if (!state->has_message && !state->finished && !state->receive_pending)
			crnet_apple_websocket_start_receive(socket);
		// With CRNET_NO_WAIT the limit has already passed and this only
		// looks at what is there.
		while (!state->has_message && !state->finished && !state->aborted) {
			if (![state->condition waitUntilDate:limit])
				break;
		}

		if (state->aborted) {
			result = CR_RESULT_NET_ABORTED;
		} else if (state->has_message) {
			NSUInteger length = state->message.length;
			NSUInteger left   = length - state->message_offset;
			NSUInteger count  = size < (uint64_t)left ? (NSUInteger)size : left;
			if (count > 0)
				memcpy(data, (const uint8_t *)state->message.bytes + state->message_offset, count);
			state->message_offset += count;
			*out_received          = count;
			*out_type              = state->message_type;
			if (state->message_offset == length) {
				*out_message_complete = true;
				[state->message release];
				state->message     = nil;
				state->has_message = NO;
			}
		} else if (!state->finished) {
			result = CR_RESULT_NET_TIMED_OUT;
		} else if (state->close_requested) {
			// This side's close, whatever the task's last receive then
			// failed with.
			result = CR_RESULT_NET_WS_CLOSED;
		} else if (state->too_big || (!state->peer_closed && socket->task.closeCode == NSURLSessionWebSocketCloseCodeMessageTooBig)) {
			// Two signs of a message over the limit, either of which will
			// do: the receive's own error, and the task having closed the
			// connection with "message too big" (1009) of its own accord.
			// The same code in a close the peer sent means this side's
			// message was too big for it, which is an ordinary close and
			// falls through to the next case.
			result = CR_RESULT_NET_WS_MESSAGE_TOO_BIG;
		} else if (state->peer_closed || state->close_requested || socket->task.closeCode != NSURLSessionWebSocketCloseCodeInvalid) {
			result = CR_RESULT_NET_WS_CLOSED;
		} else {
			result = CR_RESULT_NET_RECV_FAILED;
		}
		// What NSURLSession reported is kept for both: for a message too
		// big it is the evidence the verdict rests on.
		if ((result == CR_RESULT_NET_RECV_FAILED || result == CR_RESULT_NET_WS_MESSAGE_TOO_BIG) && state->receive_error.source != CR_PLATFORM_ERROR_SOURCE_NONE) {
			socket->errors[CRNET_ERROR_SIDE_RECEIVING]        = state->receive_error;
			socket->errors[CRNET_ERROR_SIDE_RECEIVING].result = result;
		}
		[state->condition unlock];

		return result;
	}
}

CR_RESULT crnet_websocket_close(
    crnet_websocket socket,
    uint16_t code,
    const char *reason) {
	if (!socket)
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	if (CR_STALE(socket, "crnet_websocket_close", "socket"))
		return CR_RESULT_NET_INVALID_WEBSOCKET;
	size_t reason_length = reason ? strlen(reason) : 0;
	if (reason_length > 123)
		return CR_RESULT_NET_SEND_FAILED;

	@autoreleasepool {
		CrNetWebSocketState *state = socket->state;
		[state->condition lock];
		CR_RESULT usable = crnet_apple_websocket_usable(state);
		if (!CR_FAILED(usable) && state->finished)
			usable = CR_RESULT_NET_SEND_FAILED; // already ended: no connection to send a frame on
		if (!CR_FAILED(usable)) {
			// The end of the connection for this side, whatever the task
			// still reports afterwards: a message that arrived and wasn't
			// received yet is dropped, and a receive that is waiting is
			// woken to find the socket closed.
			state->close_requested = YES;
			state->finished        = YES;
			[state->message release];
			state->message     = nil;
			state->has_message = NO;
			[state->condition broadcast];
			crnet_wait_set_notify(state->wait_set);
		}
		[state->condition unlock];
		if (CR_FAILED(usable))
			return usable;

		// Queues the close frame and returns; nothing here waits. It is
		// also the end of the task's receiving, which is what every stack's
		// close here is made to match.
		[socket->task cancelWithCloseCode:(NSURLSessionWebSocketCloseCode)code
		                           reason:(reason_length > 0 ? [NSData dataWithBytes:reason length:reason_length] : nil)];
	}
	return CR_SUCCESS;
}

uint16_t crnet_websocket_get_close_code(crnet_websocket socket) {
	if (!socket)
		return 0;
	if (CR_STALE(socket, "crnet_websocket_get_close_code", "socket"))
		return 0;
	CrNetWebSocketState *state = socket->state;
	[state->condition lock];
	// Only a close the peer made has a code to report. (The task's own
	// closeCode also holds the code this side sent, which is not one.)
	uint16_t code = 0;
	if (state->close_requested)
		code = 0;
	else if (state->peer_closed)
		code = state->close_code;
	else if (socket->task.closeCode != NSURLSessionWebSocketCloseCodeInvalid)
		code = (uint16_t)socket->task.closeCode;
	[state->condition unlock];
	return code;
}

#endif // CRNET_EXT_WEBSOCKET

#endif // CARROTTUS_PLATFORM_APPLE
