#ifndef CRMESSAGE_H
#define CRMESSAGE_H

/*
 * Sending a message to an instance's callback (crcore.h, "Messages";
 * card S10). Every module whose instance takes a callback sends through
 * cr_message_platform: the message is built on the stack, its strings are
 * the call's literals, the objects' own names and the platform's own text,
 * so sending allocates nothing (M5). No lock is held here (M4).
 */

#include "crcore.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Who a failure is reported about: the call that was running, the handle
 * it was made on and that handle's debug name, and for a create the name
 * its desc gave the object it was making. */
typedef struct cr_message_about {
	const char *call;
	const void *object;
	const char *object_debug_name;
	const char *subject_debug_name;
} cr_message_about;

/* Sends a platform failure (M2): origin PLATFORM, severity ERROR, inside
 * the failing call, on its thread (M3). [text] is the platform's own
 * wording, or NULL for none (sent as ""). Nothing is sent with no
 * callback. */
static inline void cr_message_platform(
    CR_MESSAGE_CALLBACK callback,
    void *user_data,
    const cr_message_about *about,
    CR_RESULT result,
    CR_PLATFORM_ERROR_SOURCE source,
    int64_t code,
    const char *text) {
	if (!callback)
		return;
	CR_MESSAGE message;
	message.base.type          = CR_STRUCT_TYPE_MESSAGE;
	message.base.next          = NULL;
	message.origin             = CR_MESSAGE_ORIGIN_PLATFORM;
	message.severity           = CR_MESSAGE_SEVERITY_ERROR;
	message.platform_source    = source;
	message.result             = result;
	message.id                 = 0;
	message.dropped            = 0;
	message.platform_code      = code;
	message.call               = about->call;
	message.object             = about->object;
	message.object_debug_name  = about->object_debug_name;
	message.subject_debug_name = about->subject_debug_name;
	message.text               = text ? text : "";
	callback(&message, user_data);
}

#ifdef __cplusplus
}
#endif

#endif // CRMESSAGE_H
