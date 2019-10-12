#include <common/bech32.h>
#include <common/hash_u5.h>
#include <common/json_helpers.h>
#include <common/jsonrpc_errors.h>
#include <common/param.h>
#include <common/type_to_string.h>
#include <common/utils.h>
#include <errno.h>
#include <gossipd/gen_gossip_wire.h>
#include <hsmd/gen_hsm_wire.h>
#include <string.h>
#include <wire/wire_sync.h>

/* These tables copied from zbase32 src:
 * copyright 2002-2007 Zooko "Zooko" Wilcox-O'Hearn
 * mailto:zooko@zooko.com
 *
 * Permission is hereby granted to any person obtaining a copy of this work to
 * deal in this work without restriction (including the rights to use, modify,
 * distribute, sublicense, and/or sell copies).
 */
static const char*const zbase32_chars="ybndrfg8ejkmcpqxot1uwisza345h769";

/* revchars: index into this table with the ASCII value of the char.  The result is the value of that quintet. */
static const u8 zbase32_revchars[]={ 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 18, 255, 25, 26, 27, 30, 29, 7, 31, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 24, 1, 12, 3, 8, 5, 6, 28, 21, 9, 10, 255, 11, 2, 16, 13, 14, 4, 22, 17, 19, 255, 20, 15, 0, 23, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, };

static const char *to_zbase32(const tal_t *ctx, const u8 *msg, size_t srclen)
{
	size_t outlen;
	char *out = tal_arr(ctx, char, (srclen * 8 + 4) / 5 + 1);

	outlen = 0;
	if (!bech32_convert_bits((uint8_t *)out, &outlen, 5, msg, srclen, 8, true))
		return tal_free(out);
	assert(outlen < tal_bytelen(out));
	for (size_t i = 0; i < outlen; i++)
		out[i] = zbase32_chars[(unsigned)out[i]];
	out[outlen] = '\0';
	return out;
}

static const u8 *from_zbase32(const tal_t *ctx, const char *msg)
{
	u5 *u5arr;
	u8 *u8arr;
	size_t len;

	u5arr = tal_arr(tmpctx, u5, strlen(msg));
	for (size_t i = 0; i < tal_bytelen(u5arr); i++) {
		u5arr[i] = zbase32_revchars[(unsigned char)msg[i]];
		if (u5arr[i] > 31)
			return NULL;
	}

	u8arr = tal_arr(ctx, u8, (tal_bytelen(u5arr) * 5 + 7) / 8);
	len = 0;
	if (!bech32_convert_bits(u8arr, &len, 8,
				 u5arr, tal_bytelen(u5arr), 5, false))
		return tal_free(u8arr);
	assert(len == tal_bytelen(u8arr));
	return u8arr;
}

static struct command_result *json_signmessage(struct command *cmd,
					       const char *buffer,
					       const jsmntok_t *obj UNNEEDED,
					       const jsmntok_t *params)
{
	const char *message;
	secp256k1_ecdsa_recoverable_signature rsig;
	struct json_stream *response;
	u8 sig[65], *msg;
	int recid;

	if (!param(cmd, buffer, params,
		   p_req("message", param_string, &message),
		   NULL))
		return command_param_failed();

	if (strlen(message) > 65535)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "Message must be < 64k");

	msg = towire_hsm_sign_message(NULL,
				      tal_dup_arr(tmpctx, u8, (u8 *)message,
						  strlen(message), 0));
	if (!wire_sync_write(cmd->ld->hsm_fd, take(msg)))
		fatal("Could not write to HSM: %s", strerror(errno));

	msg = wire_sync_read(tmpctx, cmd->ld->hsm_fd);
	if (!fromwire_hsm_sign_message_reply(msg, &rsig))
		fatal("HSM gave bad hsm_sign_message_reply %s",
		      tal_hex(msg, msg));

	secp256k1_ecdsa_recoverable_signature_serialize_compact(secp256k1_ctx,
								sig+1, &recid,
								&rsig);
	response = json_stream_success(cmd);
	json_add_hex(response, "signature", sig+1, sizeof(sig)-1);
	sig[0] = recid;
	json_add_hex(response, "recid", sig, 1);

	/* From https://twitter.com/rusty_twit/status/1182102005914800128:
	 * @roasbeef & @bitconner point out that #lnd algo is:
	 *   zbase32(SigRec(SHA256(SHA256("Lightning Signed Message:" + msg)))).
	 * zbase32 from https://philzimmermann.com/docs/human-oriented-base-32-encoding.txt
	 * and SigRec has first byte 31 + recovery id, followed by 64 byte sig.
	 * #specinatweet */
	sig[0] += 31;
	json_add_string(response, "zbase",
			to_zbase32(response, sig, sizeof(sig)));
	return command_success(cmd, response);
}

static const struct json_command json_signmessage_cmd = {
	"signmessage",
	"utility",
	json_signmessage,
	"Create a digital signature of {message}",
};
AUTODATA(json_command, &json_signmessage_cmd);

static struct command_result *json_checkmessage(struct command *cmd,
						const char *buffer,
						const jsmntok_t *obj UNNEEDED,
						const jsmntok_t *params)
{
	struct pubkey *pubkey, reckey;
	const u8 *u8sig;
	const char *message, *zb;
	secp256k1_ecdsa_recoverable_signature rsig;
	struct sha256_ctx sctx = SHA256_INIT;
	struct sha256_double shad;
	struct json_stream *response;

	if (!param(cmd, buffer, params,
		   p_req("message", param_string, &message),
		   p_req("zbase", param_string, &zb),
		   p_req("pubkey", param_pubkey, &pubkey),
		   NULL))
		return command_param_failed();

	u8sig = from_zbase32(tmpctx, zb);
	if (!u8sig)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "zbase is not valid zbase32");

	if (tal_bytelen(u8sig) != 65)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "zbase is too %s",
				    tal_bytelen(u8sig) < 65 ? "short" : "long");

	if (!secp256k1_ecdsa_recoverable_signature_parse_compact(secp256k1_ctx,
								 &rsig,
								 u8sig + 1,
								 u8sig[0] - 31))
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "cannot parse zbase signature");

	sha256_update(&sctx, "Lightning Signed Message:",
		      strlen("Lightning Signed Message:"));
	sha256_update(&sctx, message, strlen(message));
	sha256_double_done(&sctx, &shad);

	response = json_stream_success(cmd);
	if (!secp256k1_ecdsa_recover(secp256k1_ctx, &reckey.pubkey, &rsig,
				     shad.sha.u.u8)) {
		json_add_bool(response, "verified", false);
	} else {
		json_add_bool(response, "verified", pubkey_eq(pubkey, &reckey));
	}
	return command_success(cmd, response);
}

static const struct json_command json_checkmessage_cmd = {
	"checkmessage",
	"utility",
	json_checkmessage,
	"Verify a digital signature {zbase} of {message} signed with {pubkey}",
};
AUTODATA(json_command, &json_checkmessage_cmd);
