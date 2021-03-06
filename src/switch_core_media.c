/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * 
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 * switch_core_media.c -- Core Media
 *
 */

#include <switch.h>
#include <switch_ssl.h>
#include <switch_stun.h>
#include <switch_nat.h>
#include <switch_version.h>
#include "private/switch_core_pvt.h"
#include <switch_curl.h>
#include <errno.h>
#include <sofia-sip/sdp.h>
#include <sofia-sip/su.h>

SWITCH_DECLARE(switch_t38_options_t *) switch_core_media_process_udptl(switch_core_session_t *session, sdp_session_t *sdp, sdp_media_t *m);
SWITCH_DECLARE(void) switch_core_media_find_zrtp_hash(switch_core_session_t *session, sdp_session_t *sdp);
SWITCH_DECLARE(void) switch_core_media_set_r_sdp_codec_string(switch_core_session_t *session, const char *codec_string, sdp_session_t *sdp);

//#define GOOGLE_ICE
#define RTCP_MUX
#define MAX_CODEC_CHECK_FRAMES 50//x:mod_sofia.h
#define MAX_MISMATCH_FRAMES 5//x:mod_sofia.h
#define type2str(type) type == SWITCH_MEDIA_TYPE_VIDEO ? "video" : "audio"

typedef enum {
	SMF_INIT = (1 << 0),
	SMF_READY = (1 << 1),
	SMF_JB_PAUSED = (1 << 2)
} smh_flag_t;


typedef struct secure_settings_s {
	int crypto_tag;
	unsigned char local_raw_key[SWITCH_RTP_MAX_CRYPTO_LEN];
	unsigned char remote_raw_key[SWITCH_RTP_MAX_CRYPTO_LEN];
	switch_rtp_crypto_key_type_t crypto_send_type;
	switch_rtp_crypto_key_type_t crypto_recv_type;
	switch_rtp_crypto_key_type_t crypto_type;
	char *local_crypto_key;
	char *remote_crypto_key;
} switch_secure_settings_t;

typedef struct codec_params_s {
	char *rm_encoding;
	char *iananame;
	switch_payload_t pt;
	unsigned long rm_rate;
	uint32_t codec_ms;
	uint32_t bitrate;

	char *rm_fmtp;

	switch_payload_t agreed_pt;
	switch_payload_t recv_pt;
	char *fmtp_out;


	char *remote_sdp_ip;
	switch_port_t remote_sdp_port;

	char *local_sdp_ip;
	switch_port_t local_sdp_port;

	char *adv_sdp_ip;
	switch_port_t adv_sdp_port;
	char *proxy_sdp_ip;
	switch_port_t proxy_sdp_port;
	int channels;
	int adv_channels;

} codec_params_t;

struct media_helper {
	switch_core_session_t *session;
	switch_thread_cond_t *cond;
	switch_mutex_t *cond_mutex;
	int up;
};

typedef struct switch_rtp_engine_s {
	switch_secure_settings_t ssec;
	switch_media_type_t type;

	switch_rtp_t *rtp_session;
	switch_frame_t read_frame;
	switch_codec_t read_codec;
	switch_codec_t write_codec;

	switch_codec_implementation_t read_impl;
	switch_codec_implementation_t write_impl;

	uint32_t codec_ms;
	switch_size_t last_ts;
	uint32_t check_frames;
	uint32_t mismatch_count;
	uint32_t last_codec_ms;
	uint8_t codec_reinvites;
	uint32_t max_missed_packets;
	uint32_t max_missed_hold_packets;
	uint32_t ssrc;
	uint32_t remote_ssrc;
	switch_port_t remote_rtcp_port;
	switch_rtp_bug_flag_t rtp_bugs;

	/** ZRTP **/
	char *local_sdp_zrtp_hash;
	char *remote_sdp_zrtp_hash;


	codec_params_t codec_params;
	uint32_t timestamp_send;

	char *cand_acl[SWITCH_MAX_CAND_ACL];
	int cand_acl_count;

	ice_t ice_in;
	ice_t ice_out;

	int8_t rtcp_mux;

	dtls_fingerprint_t local_dtls_fingerprint;
	dtls_fingerprint_t remote_dtls_fingerprint;

	char *remote_rtp_ice_addr;
	switch_port_t remote_rtp_ice_port;

	char *remote_rtcp_ice_addr;
	switch_port_t remote_rtcp_ice_port;

	struct media_helper mh;
	switch_thread_t *media_thread;

} switch_rtp_engine_t;


struct switch_media_handle_s {
	switch_core_session_t *session;
	switch_channel_t *channel;
	switch_core_media_flag_t media_flags[SCMF_MAX];
	smh_flag_t flags;
	switch_rtp_engine_t engines[SWITCH_MEDIA_TYPE_TOTAL];

	char *codec_order[SWITCH_MAX_CODECS];
    int codec_order_last;
    const switch_codec_implementation_t *codecs[SWITCH_MAX_CODECS];

	int payload_space;
	char *origin;

	switch_mutex_t *mutex;

	const switch_codec_implementation_t *negotiated_codecs[SWITCH_MAX_CODECS];
	int num_negotiated_codecs;
	switch_payload_t ianacodes[SWITCH_MAX_CODECS];
	int video_count;

	uint32_t owner_id;
	uint32_t session_id;

	switch_core_media_params_t *mparams;

	char *msid;
	char *cname;

};

static int get_channels(const switch_codec_implementation_t *imp)
{
	if (!strcasecmp(imp->iananame, "opus")) {
		return 2; /* IKR???*/
	}

	return imp->number_of_channels;
}

static void _switch_core_media_pass_zrtp_hash2(switch_core_session_t *aleg_session, switch_core_session_t *bleg_session, switch_media_type_t type)
{
	switch_rtp_engine_t *aleg_engine;
	switch_rtp_engine_t *bleg_engine;

	if (!aleg_session->media_handle || !bleg_session->media_handle) return;
	aleg_engine = &aleg_session->media_handle->engines[type];
	bleg_engine = &bleg_session->media_handle->engines[type];



	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(aleg_session->channel), SWITCH_LOG_DEBUG1, 
					  "Deciding whether to pass zrtp-hash between a-leg and b-leg\n");

	if (!(switch_channel_test_flag(aleg_session->channel, CF_ZRTP_PASSTHRU_REQ))) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(aleg_session->channel), SWITCH_LOG_DEBUG1, 
						  "CF_ZRTP_PASSTHRU_REQ not set on a-leg, so not propagating zrtp-hash\n");
		return;
	}

	if (aleg_engine->remote_sdp_zrtp_hash) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(aleg_session->channel), SWITCH_LOG_DEBUG, "Passing a-leg remote zrtp-hash (audio) to b-leg\n");
		bleg_engine->local_sdp_zrtp_hash = switch_core_session_strdup(bleg_session, aleg_engine->remote_sdp_zrtp_hash);
		switch_channel_set_variable(bleg_session->channel, "l_sdp_audio_zrtp_hash", bleg_engine->local_sdp_zrtp_hash);
	}
	
	if (bleg_engine->remote_sdp_zrtp_hash) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(aleg_session->channel), SWITCH_LOG_DEBUG, "Passing b-leg remote zrtp-hash (audio) to a-leg\n");
		aleg_engine->local_sdp_zrtp_hash = switch_core_session_strdup(aleg_session, bleg_engine->remote_sdp_zrtp_hash);
		switch_channel_set_variable(aleg_session->channel, "l_sdp_audio_zrtp_hash", aleg_engine->local_sdp_zrtp_hash);
	}
}

SWITCH_DECLARE(void) switch_core_media_pass_zrtp_hash2(switch_core_session_t *aleg_session, switch_core_session_t *bleg_session)
{
	_switch_core_media_pass_zrtp_hash2(aleg_session, bleg_session, SWITCH_MEDIA_TYPE_AUDIO);
	_switch_core_media_pass_zrtp_hash2(aleg_session, bleg_session, SWITCH_MEDIA_TYPE_VIDEO);
}


SWITCH_DECLARE(void) switch_core_media_pass_zrtp_hash(switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);

	switch_core_session_t *other_session;
	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "Deciding whether to pass zrtp-hash between legs\n");
	if (!(switch_channel_test_flag(channel, CF_ZRTP_PASSTHRU_REQ))) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "CF_ZRTP_PASSTHRU_REQ not set, so not propagating zrtp-hash\n");
		return;
	} else if (!(switch_core_session_get_partner(session, &other_session) == SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "No partner channel found, so not propagating zrtp-hash\n");
		return;
	} else {
		switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "Found peer channel; propagating zrtp-hash if set\n");
		switch_core_media_pass_zrtp_hash2(session, other_session);
		switch_core_session_rwunlock(other_session);
	}
}

SWITCH_DECLARE(const char *) switch_core_media_get_zrtp_hash(switch_core_session_t *session, switch_media_type_t type, switch_bool_t local)
{
	switch_rtp_engine_t *engine;
	if (!session->media_handle) return NULL;

	engine = &session->media_handle->engines[type];

	if (local) {
		return engine->local_sdp_zrtp_hash;
	}

	
	return engine->remote_sdp_zrtp_hash;

}

SWITCH_DECLARE(void) switch_core_media_find_zrtp_hash(switch_core_session_t *session, sdp_session_t *sdp)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_rtp_engine_t *audio_engine;
	switch_rtp_engine_t *video_engine;
	sdp_media_t *m;
	sdp_attribute_t *attr;
	int got_audio = 0, got_video = 0;

	if (!session->media_handle) return;

	audio_engine = &session->media_handle->engines[SWITCH_MEDIA_TYPE_AUDIO];
	video_engine = &session->media_handle->engines[SWITCH_MEDIA_TYPE_VIDEO];


	switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG1, "Looking for zrtp-hash\n");
	for (m = sdp->sdp_media; m; m = m->m_next) {
		if (got_audio && got_video) break;
		if (m->m_port && ((m->m_type == sdp_media_audio && !got_audio)
						  || (m->m_type == sdp_media_video && !got_video))) {
			for (attr = m->m_attributes; attr; attr = attr->a_next) {
				if (zstr(attr->a_name)) continue;
				if (strcasecmp(attr->a_name, "zrtp-hash") || !(attr->a_value)) continue;
				if (m->m_type == sdp_media_audio) {
					switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG,
									  "Found audio zrtp-hash; setting r_sdp_audio_zrtp_hash=%s\n", attr->a_value);
					switch_channel_set_variable(channel, "r_sdp_audio_zrtp_hash", attr->a_value);
					audio_engine->remote_sdp_zrtp_hash = switch_core_session_strdup(session, attr->a_value);
					got_audio++;
				} else if (m->m_type == sdp_media_video) {
					switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_DEBUG,
									  "Found video zrtp-hash; setting r_sdp_video_zrtp_hash=%s\n", attr->a_value);
					switch_channel_set_variable(channel, "r_sdp_video_zrtp_hash", attr->a_value);
					video_engine->remote_sdp_zrtp_hash = switch_core_session_strdup(session, attr->a_value);
					got_video++;
				}
				switch_channel_set_flag(channel, CF_ZRTP_HASH);
				break;
			}
		}
	}
}


SWITCH_DECLARE(switch_t38_options_t *) switch_core_media_process_udptl(switch_core_session_t *session, sdp_session_t *sdp, sdp_media_t *m)
{
	switch_t38_options_t *t38_options = switch_channel_get_private(session->channel, "t38_options");
	sdp_attribute_t *attr;

	if (!t38_options) {
		t38_options = switch_core_session_alloc(session, sizeof(switch_t38_options_t));

		// set some default value
		t38_options->T38FaxVersion = 0;
		t38_options->T38MaxBitRate = 14400;
		t38_options->T38FaxRateManagement = switch_core_session_strdup(session, "transferredTCF");
		t38_options->T38FaxUdpEC = switch_core_session_strdup(session, "t38UDPRedundancy");
		t38_options->T38FaxMaxBuffer = 500;
		t38_options->T38FaxMaxDatagram = 500;
	}

	t38_options->remote_port = (switch_port_t)m->m_port;

	if (sdp->sdp_origin) {
		t38_options->sdp_o_line = switch_core_session_strdup(session, sdp->sdp_origin->o_username);
	} else {
		t38_options->sdp_o_line = "unknown";
	}
	
	if (m->m_connections && m->m_connections->c_address) {
		t38_options->remote_ip = switch_core_session_strdup(session, m->m_connections->c_address);
	} else if (sdp && sdp->sdp_connection && sdp->sdp_connection->c_address) {
		t38_options->remote_ip = switch_core_session_strdup(session, sdp->sdp_connection->c_address);
	}

	for (attr = m->m_attributes; attr; attr = attr->a_next) {
		if (!strcasecmp(attr->a_name, "T38FaxVersion") && attr->a_value) {
			t38_options->T38FaxVersion = (uint16_t) atoi(attr->a_value);
		} else if (!strcasecmp(attr->a_name, "T38MaxBitRate") && attr->a_value) {
			t38_options->T38MaxBitRate = (uint32_t) atoi(attr->a_value);
		} else if (!strcasecmp(attr->a_name, "T38FaxFillBitRemoval")) {
			t38_options->T38FaxFillBitRemoval = switch_safe_atoi(attr->a_value, 1);
		} else if (!strcasecmp(attr->a_name, "T38FaxTranscodingMMR")) {
			t38_options->T38FaxTranscodingMMR = switch_safe_atoi(attr->a_value, 1);
		} else if (!strcasecmp(attr->a_name, "T38FaxTranscodingJBIG")) {
			t38_options->T38FaxTranscodingJBIG = switch_safe_atoi(attr->a_value, 1);
		} else if (!strcasecmp(attr->a_name, "T38FaxRateManagement") && attr->a_value) {
			t38_options->T38FaxRateManagement = switch_core_session_strdup(session, attr->a_value);
		} else if (!strcasecmp(attr->a_name, "T38FaxMaxBuffer") && attr->a_value) {
			t38_options->T38FaxMaxBuffer = (uint32_t) atoi(attr->a_value);
		} else if (!strcasecmp(attr->a_name, "T38FaxMaxDatagram") && attr->a_value) {
			t38_options->T38FaxMaxDatagram = (uint32_t) atoi(attr->a_value);
		} else if (!strcasecmp(attr->a_name, "T38FaxUdpEC") && attr->a_value) {
			t38_options->T38FaxUdpEC = switch_core_session_strdup(session, attr->a_value);
		} else if (!strcasecmp(attr->a_name, "T38VendorInfo") && attr->a_value) {
			t38_options->T38VendorInfo = switch_core_session_strdup(session, attr->a_value);
		}
	}

	switch_channel_set_variable(session->channel, "has_t38", "true");
	switch_channel_set_private(session->channel, "t38_options", t38_options);
	switch_channel_set_app_flag_key("T38", session->channel, CF_APP_T38);

	switch_channel_execute_on(session->channel, "sip_execute_on_image");
	switch_channel_api_on(session->channel, "sip_api_on_image");

	return t38_options;
}





SWITCH_DECLARE(switch_t38_options_t *) switch_core_media_extract_t38_options(switch_core_session_t *session, const char *r_sdp)
{
	sdp_media_t *m;
	sdp_parser_t *parser = NULL;
	sdp_session_t *sdp;
	switch_t38_options_t *t38_options = NULL;

	if (!(parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
		return NULL;
	}

	if (!(sdp = sdp_session(parser))) {
		sdp_parser_free(parser);
		return NULL;
	}

	for (m = sdp->sdp_media; m; m = m->m_next) {
		if (m->m_proto == sdp_proto_udptl && m->m_type == sdp_media_image && m->m_port) {
			t38_options = switch_core_media_process_udptl(session, sdp, m);
			break;
		}
	}

	sdp_parser_free(parser);

	return t38_options;

}



//?
SWITCH_DECLARE(switch_status_t) switch_core_media_process_t38_passthru(switch_core_session_t *session, 
																	   switch_core_session_t *other_session, switch_t38_options_t *t38_options)
{
	char *remote_host;
	switch_port_t remote_port;
	char tmp[32] = "";
	switch_rtp_engine_t *a_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];

	remote_host = switch_rtp_get_remote_host(a_engine->rtp_session);
	remote_port = switch_rtp_get_remote_port(a_engine->rtp_session);
	
	a_engine->codec_params.remote_sdp_ip = switch_core_session_strdup(session, t38_options->remote_ip);
	a_engine->codec_params.remote_sdp_port = t38_options->remote_port;
							
	if (remote_host && remote_port && !strcmp(remote_host, a_engine->codec_params.remote_sdp_ip) && 
		remote_port == a_engine->codec_params.remote_sdp_port) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
						  "Audio params are unchanged for %s.\n",
						  switch_channel_get_name(session->channel));
	} else {
		const char *err = NULL;
			
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
						  "Audio params changed for %s from %s:%d to %s:%d\n",
						  switch_channel_get_name(session->channel),
						  remote_host, remote_port, a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);
			
		switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.remote_sdp_port);
		switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE, a_engine->codec_params.remote_sdp_ip);
		switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE, tmp);
		if (switch_rtp_set_remote_address(a_engine->rtp_session, a_engine->codec_params.remote_sdp_ip,
										  a_engine->codec_params.remote_sdp_port, 0, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AUDIO RTP REPORTS ERROR: [%s]\n", err);
			switch_channel_hangup(session->channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		}
	}
		
	switch_core_media_copy_t38_options(t38_options, other_session);

	return SWITCH_STATUS_SUCCESS;

}






SWITCH_DECLARE(const char *)switch_core_media_get_codec_string(switch_core_session_t *session)
{
	const char *preferred = NULL, *fallback = NULL;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		preferred = "PCMU";
		fallback = "PCMU";
	} else {
	
		if (!(preferred = switch_channel_get_variable(session->channel, "absolute_codec_string"))) {
			preferred = switch_channel_get_variable(session->channel, "codec_string");
		}
	
		if (!preferred) {
			if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
				preferred = smh->mparams->outbound_codec_string;
				fallback = smh->mparams->inbound_codec_string;

			} else {
				preferred = smh->mparams->inbound_codec_string;
				fallback = smh->mparams->outbound_codec_string;
			}
		}
	}

	return !zstr(preferred) ? preferred : fallback;
}


SWITCH_DECLARE(const char *) switch_core_session_local_crypto_key(switch_core_session_t *session, switch_media_type_t type)
{
	if (!session->media_handle) {
		return NULL;
	}

	return session->media_handle->engines[type].ssec.local_crypto_key;
}



SWITCH_DECLARE(void) switch_core_media_parse_rtp_bugs(switch_rtp_bug_flag_t *flag_pole, const char *str)
{

	if (switch_stristr("clear", str)) {
		*flag_pole = 0;
	}

	if (switch_stristr("CISCO_SKIP_MARK_BIT_2833", str)) {
		*flag_pole |= RTP_BUG_CISCO_SKIP_MARK_BIT_2833;
	}

	if (switch_stristr("~CISCO_SKIP_MARK_BIT_2833", str)) {
		*flag_pole &= ~RTP_BUG_CISCO_SKIP_MARK_BIT_2833;
	}

	if (switch_stristr("SONUS_SEND_INVALID_TIMESTAMP_2833", str)) {
		*flag_pole |= RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833;
	}

	if (switch_stristr("~SONUS_SEND_INVALID_TIMESTAMP_2833", str)) {
		*flag_pole &= ~RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833;
	}

	if (switch_stristr("IGNORE_MARK_BIT", str)) {
		*flag_pole |= RTP_BUG_IGNORE_MARK_BIT;
	}	

	if (switch_stristr("~IGNORE_MARK_BIT", str)) {
		*flag_pole &= ~RTP_BUG_IGNORE_MARK_BIT;
	}	

	if (switch_stristr("SEND_LINEAR_TIMESTAMPS", str)) {
		*flag_pole |= RTP_BUG_SEND_LINEAR_TIMESTAMPS;
	}

	if (switch_stristr("~SEND_LINEAR_TIMESTAMPS", str)) {
		*flag_pole &= ~RTP_BUG_SEND_LINEAR_TIMESTAMPS;
	}

	if (switch_stristr("START_SEQ_AT_ZERO", str)) {
		*flag_pole |= RTP_BUG_START_SEQ_AT_ZERO;
	}

	if (switch_stristr("~START_SEQ_AT_ZERO", str)) {
		*flag_pole &= ~RTP_BUG_START_SEQ_AT_ZERO;
	}

	if (switch_stristr("NEVER_SEND_MARKER", str)) {
		*flag_pole |= RTP_BUG_NEVER_SEND_MARKER;
	}

	if (switch_stristr("~NEVER_SEND_MARKER", str)) {
		*flag_pole &= ~RTP_BUG_NEVER_SEND_MARKER;
	}

	if (switch_stristr("IGNORE_DTMF_DURATION", str)) {
		*flag_pole |= RTP_BUG_IGNORE_DTMF_DURATION;
	}

	if (switch_stristr("~IGNORE_DTMF_DURATION", str)) {
		*flag_pole &= ~RTP_BUG_IGNORE_DTMF_DURATION;
	}

	if (switch_stristr("ACCEPT_ANY_PACKETS", str)) {
		*flag_pole |= RTP_BUG_ACCEPT_ANY_PACKETS;
	}

	if (switch_stristr("~ACCEPT_ANY_PACKETS", str)) {
		*flag_pole &= ~RTP_BUG_ACCEPT_ANY_PACKETS;
	}

	if (switch_stristr("GEN_ONE_GEN_ALL", str)) {
		*flag_pole |= RTP_BUG_GEN_ONE_GEN_ALL;
	}

	if (switch_stristr("~GEN_ONE_GEN_ALL", str)) {
		*flag_pole &= ~RTP_BUG_GEN_ONE_GEN_ALL;
	}

	if (switch_stristr("CHANGE_SSRC_ON_MARKER", str)) {
		*flag_pole |= RTP_BUG_CHANGE_SSRC_ON_MARKER;
	}

	if (switch_stristr("~CHANGE_SSRC_ON_MARKER", str)) {
		*flag_pole &= ~RTP_BUG_CHANGE_SSRC_ON_MARKER;
	}

	if (switch_stristr("FLUSH_JB_ON_DTMF", str)) {
		*flag_pole |= RTP_BUG_FLUSH_JB_ON_DTMF;
	}

	if (switch_stristr("~FLUSH_JB_ON_DTMF", str)) {
		*flag_pole &= ~RTP_BUG_FLUSH_JB_ON_DTMF;
	}
}


static switch_status_t switch_core_media_build_crypto(switch_media_handle_t *smh,
													  switch_media_type_t type,
													  int index, switch_rtp_crypto_key_type_t ctype, switch_rtp_crypto_direction_t direction, int force)
{
	unsigned char b64_key[512] = "";
	const char *type_str;
	unsigned char *key;
	const char *val;
	switch_channel_t *channel;
	char *p;
	switch_rtp_engine_t *engine;

	switch_assert(smh);
	channel = switch_core_session_get_channel(smh->session);

	engine = &smh->engines[type];

	if (!force && engine->ssec.local_raw_key[0]) {
		return SWITCH_STATUS_SUCCESS;
	}

	if (ctype == AES_CM_128_HMAC_SHA1_80) {
		type_str = SWITCH_RTP_CRYPTO_KEY_80;
	} else {
		type_str = SWITCH_RTP_CRYPTO_KEY_32;
	}

//#define SAME_KEY
#ifdef SAME_KEY
	if (switch_channel_test_flag(channel, CF_WEBRTC) && type == SWITCH_MEDIA_TYPE_VIDEO) {
		if (direction == SWITCH_RTP_CRYPTO_SEND) {
			memcpy(engine->ssec.local_raw_key, smh->engines[SWITCH_MEDIA_TYPE_AUDIO].ssec.local_raw_key, SWITCH_RTP_KEY_LEN);
			key = engine->ssec.local_raw_key;
		} else {
			memcpy(engine->ssec.remote_raw_key, smh->engines[SWITCH_MEDIA_TYPE_AUDIO].ssec.remote_raw_key, SWITCH_RTP_KEY_LEN);
			key = engine->ssec.remote_raw_key;
		}
	} else {
#endif
		if (direction == SWITCH_RTP_CRYPTO_SEND) {
			key = engine->ssec.local_raw_key;
		} else {
			key = engine->ssec.remote_raw_key;
		}
		
		switch_rtp_get_random(key, SWITCH_RTP_KEY_LEN);
#ifdef SAME_KEY
	}
#endif

	switch_b64_encode(key, SWITCH_RTP_KEY_LEN, b64_key, sizeof(b64_key));
	p = strrchr((char *) b64_key, '=');

	while (p && *p && *p == '=') {
		*p-- = '\0';
	}

	engine->ssec.local_crypto_key = switch_core_session_sprintf(smh->session, "%d %s inline:%s", index, type_str, b64_key);
	switch_channel_set_variable_name_printf(smh->session->channel, engine->ssec.local_crypto_key, "rtp_last_%s_local_crypto_key", type2str(type));


	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_DEBUG, "Set Local Key [%s]\n", engine->ssec.local_crypto_key);

	if (!(smh->mparams->ndlb & SM_NDLB_DISABLE_SRTP_AUTH) &&
		!((val = switch_channel_get_variable(channel, "NDLB_support_asterisk_missing_srtp_auth")) && switch_true(val))) {
		engine->ssec.crypto_type = ctype;
	} else {
		engine->ssec.crypto_type = AES_CM_128_NULL_AUTH;
	}

	return SWITCH_STATUS_SUCCESS;
}





switch_status_t switch_core_media_add_crypto(switch_secure_settings_t *ssec, const char *key_str, switch_rtp_crypto_direction_t direction)
{
	unsigned char key[SWITCH_RTP_MAX_CRYPTO_LEN];
	switch_rtp_crypto_key_type_t type;
	char *p;


	p = strchr(key_str, ' ');

	if (p && *p && *(p + 1)) {
		p++;
		if (!strncasecmp(p, SWITCH_RTP_CRYPTO_KEY_32, strlen(SWITCH_RTP_CRYPTO_KEY_32))) {
			type = AES_CM_128_HMAC_SHA1_32;
		} else if (!strncasecmp(p, SWITCH_RTP_CRYPTO_KEY_80, strlen(SWITCH_RTP_CRYPTO_KEY_80))) {
			type = AES_CM_128_HMAC_SHA1_80;
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Parse Error near [%s]\n", p);
			goto bad;
		}

		p = strchr(p, ' ');
		if (p && *p && *(p + 1)) {
			p++;
			if (strncasecmp(p, "inline:", 7)) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Parse Error near [%s]\n", p);
				goto bad;
			}

			p += 7;
			switch_b64_decode(p, (char *) key, sizeof(key));

			if (direction == SWITCH_RTP_CRYPTO_SEND) {
				ssec->crypto_send_type = type;
				memcpy(ssec->local_raw_key, key, SWITCH_RTP_KEY_LEN);
			} else {
				ssec->crypto_recv_type = type;
				memcpy(ssec->remote_raw_key, key, SWITCH_RTP_KEY_LEN);
			}
			return SWITCH_STATUS_SUCCESS;
		}

	}

 bad:

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error!\n");
	return SWITCH_STATUS_FALSE;

}

SWITCH_DECLARE(void) switch_core_media_set_rtp_session(switch_core_session_t *session, switch_media_type_t type, switch_rtp_t *rtp_session)
{
	switch_rtp_engine_t *engine;
	if (!session->media_handle) return;
	engine = &session->media_handle->engines[type];
	engine->rtp_session = rtp_session;
	engine->type = type;
}


static void switch_core_session_get_recovery_crypto_key(switch_core_session_t *session, switch_media_type_t type)
{
	const char *tmp;
	switch_rtp_engine_t *engine;
	char *keyvar, *tagvar;

	if (!session->media_handle) return;
	engine = &session->media_handle->engines[type];

	if (type == SWITCH_MEDIA_TYPE_AUDIO) {
		keyvar = "srtp_remote_audio_crypto_key";
		tagvar = "srtp_remote_audio_crypto_tag";
	} else {
		keyvar = "srtp_remote_video_crypto_key";
		tagvar = "srtp_remote_video_crypto_tag";
	}


	if ((tmp = switch_channel_get_variable(session->channel, keyvar))) {
		engine->ssec.remote_crypto_key = switch_core_session_strdup(session, tmp);

		if ((tmp = switch_channel_get_variable(session->channel, tagvar))) {
			int tv = atoi(tmp);
			engine->ssec.crypto_tag = tv;
		} else {
			engine->ssec.crypto_tag = 1;
		}

		switch_channel_set_flag(session->channel, CF_SECURE);
	}
}


static void switch_core_session_apply_crypto(switch_core_session_t *session, switch_media_type_t type)
{
	switch_rtp_engine_t *engine;
	const char *varname;

	if (type == SWITCH_MEDIA_TYPE_AUDIO) {
		varname = "rtp_secure_audio_confirmed";
	} else {
		varname = "rtp_secure_video_confirmed";
	}

	if (!session->media_handle) return;
	engine = &session->media_handle->engines[type];

	if (switch_channel_test_flag(session->channel, CF_RECOVERING)) {
		return;
	}

	if (engine->ssec.remote_crypto_key && switch_channel_test_flag(session->channel, CF_SECURE)) {
		switch_core_media_add_crypto(&engine->ssec, engine->ssec.remote_crypto_key, SWITCH_RTP_CRYPTO_RECV);

		
		switch_rtp_add_crypto_key(engine->rtp_session, SWITCH_RTP_CRYPTO_SEND, 1,
								  engine->ssec.crypto_type, engine->ssec.local_raw_key, SWITCH_RTP_KEY_LEN);

		switch_rtp_add_crypto_key(engine->rtp_session, SWITCH_RTP_CRYPTO_RECV, engine->ssec.crypto_tag,
								  engine->ssec.crypto_type, engine->ssec.remote_raw_key, SWITCH_RTP_KEY_LEN);

		switch_channel_set_variable(session->channel, varname, "true");
	}

}


SWITCH_DECLARE(int) switch_core_session_check_incoming_crypto(switch_core_session_t *session, 
															   const char *varname,
															  switch_media_type_t type, const char *crypto, int crypto_tag, switch_sdp_type_t sdp_type)
{
	int got_crypto = 0;

	switch_rtp_engine_t *engine;
	if (!session->media_handle) return 0;
	engine = &session->media_handle->engines[type];

	if (engine->ssec.remote_crypto_key && switch_rtp_ready(engine->rtp_session)) {
		/* Compare all the key. The tag may remain the same even if key changed */
		if (crypto && !strcmp(crypto, engine->ssec.remote_crypto_key)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Existing key is still valid.\n");
		} else {
			const char *a = switch_stristr("AES", engine->ssec.remote_crypto_key);
			const char *b = switch_stristr("AES", crypto);

			/* Change our key every time we can */
							
			if (sdp_type == SDP_TYPE_REQUEST) {
				if (switch_stristr(SWITCH_RTP_CRYPTO_KEY_32, crypto)) {
					switch_channel_set_variable(session->channel, varname, SWITCH_RTP_CRYPTO_KEY_32);
				
					switch_core_media_build_crypto(session->media_handle, type, crypto_tag, AES_CM_128_HMAC_SHA1_32, SWITCH_RTP_CRYPTO_SEND, 1);
					switch_rtp_add_crypto_key(engine->rtp_session, SWITCH_RTP_CRYPTO_SEND, atoi(crypto), engine->ssec.crypto_type,
											  engine->ssec.local_raw_key, SWITCH_RTP_KEY_LEN);
				} else if (switch_stristr(SWITCH_RTP_CRYPTO_KEY_80, crypto)) {
					switch_channel_set_variable(session->channel, varname, SWITCH_RTP_CRYPTO_KEY_80);
					switch_core_media_build_crypto(session->media_handle, type, crypto_tag, AES_CM_128_HMAC_SHA1_80, SWITCH_RTP_CRYPTO_SEND, 1);
					switch_rtp_add_crypto_key(engine->rtp_session, SWITCH_RTP_CRYPTO_SEND, atoi(crypto), engine->ssec.crypto_type,
											  engine->ssec.local_raw_key, SWITCH_RTP_KEY_LEN);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Crypto Setup Failed!.\n");
				}
			}

			if (a && b && !strncasecmp(a, b, 23)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Change Remote key to [%s]\n", crypto);
				engine->ssec.remote_crypto_key = switch_core_session_strdup(session, crypto);
				switch_channel_set_variable(session->channel, "srtp_remote_audio_crypto_key", crypto);
				switch_channel_set_variable_printf(session->channel, "srtp_remote_audio_crypto_tag", "%d", crypto_tag);
				engine->ssec.crypto_tag = crypto_tag;
								
				if (switch_rtp_ready(engine->rtp_session) && switch_channel_test_flag(session->channel, CF_SECURE)) {
					switch_core_media_add_crypto(&engine->ssec, engine->ssec.remote_crypto_key, SWITCH_RTP_CRYPTO_RECV);
					switch_rtp_add_crypto_key(engine->rtp_session, SWITCH_RTP_CRYPTO_RECV, engine->ssec.crypto_tag,
											  engine->ssec.crypto_type, engine->ssec.remote_raw_key, SWITCH_RTP_KEY_LEN);
				}
				got_crypto++;
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Ignoring unacceptable key\n");
			}
		}
	} else if (!switch_rtp_ready(engine->rtp_session)) {
		engine->ssec.remote_crypto_key = switch_core_session_strdup(session, crypto);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set Remote Key [%s]\n", engine->ssec.remote_crypto_key);
		switch_channel_set_variable(session->channel, "srtp_remote_audio_crypto_key", crypto);
		engine->ssec.crypto_tag = crypto_tag;
		got_crypto++;

		if (zstr(engine->ssec.local_crypto_key)) {
			if (switch_stristr(SWITCH_RTP_CRYPTO_KEY_32, crypto)) {
				switch_channel_set_variable(session->channel, varname, SWITCH_RTP_CRYPTO_KEY_32);
				switch_core_media_build_crypto(session->media_handle, type, crypto_tag, AES_CM_128_HMAC_SHA1_32, SWITCH_RTP_CRYPTO_SEND, 1);
			} else if (switch_stristr(SWITCH_RTP_CRYPTO_KEY_80, crypto)) {
				switch_channel_set_variable(session->channel, varname, SWITCH_RTP_CRYPTO_KEY_80);
				switch_core_media_build_crypto(session->media_handle, type, crypto_tag, AES_CM_128_HMAC_SHA1_80, SWITCH_RTP_CRYPTO_SEND, 1);
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Crypto Setup Failed!.\n");
			}
		}
	}	

	return got_crypto;
}


SWITCH_DECLARE(void) switch_core_session_check_outgoing_crypto(switch_core_session_t *session, const char *sec_var)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *var;

	if (!switch_core_session_media_handle_ready(session) == SWITCH_STATUS_SUCCESS) {
		return;
	}
	
	if ((var = switch_channel_get_variable(channel, sec_var)) && !zstr(var)) {
		if (switch_true(var) || !strcasecmp(var, SWITCH_RTP_CRYPTO_KEY_32)) {

			switch_channel_set_flag(channel, CF_SECURE);
			switch_core_media_build_crypto(session->media_handle,
										   SWITCH_MEDIA_TYPE_AUDIO, 1, AES_CM_128_HMAC_SHA1_32, SWITCH_RTP_CRYPTO_SEND, 0);
			switch_core_media_build_crypto(session->media_handle,
										   SWITCH_MEDIA_TYPE_VIDEO, 1, AES_CM_128_HMAC_SHA1_32, SWITCH_RTP_CRYPTO_SEND, 0);
		} else if (!strcasecmp(var, SWITCH_RTP_CRYPTO_KEY_80)) {
			switch_channel_set_flag(channel, CF_SECURE);
			switch_core_media_build_crypto(session->media_handle,
										   SWITCH_MEDIA_TYPE_AUDIO, 1, AES_CM_128_HMAC_SHA1_80, SWITCH_RTP_CRYPTO_SEND, 0);
			switch_core_media_build_crypto(session->media_handle,
										   SWITCH_MEDIA_TYPE_VIDEO, 1, AES_CM_128_HMAC_SHA1_80, SWITCH_RTP_CRYPTO_SEND, 0);
		}
	}
	
}

#define add_stat(_i, _s)												\
	switch_snprintf(var_name, sizeof(var_name), "rtp_%s_%s", switch_str_nil(prefix), _s) ; \
	switch_snprintf(var_val, sizeof(var_val), "%" SWITCH_SIZE_T_FMT, _i); \
	switch_channel_set_variable(channel, var_name, var_val)

static void set_stats(switch_core_session_t *session, switch_media_type_t type, const char *prefix)
{
	switch_rtp_stats_t *stats = switch_core_media_get_stats(session, type, NULL);
	switch_channel_t *channel = switch_core_session_get_channel(session);

	char var_name[256] = "", var_val[35] = "";

	if (stats) {

		add_stat(stats->inbound.raw_bytes, "in_raw_bytes");
		add_stat(stats->inbound.media_bytes, "in_media_bytes");
		add_stat(stats->inbound.packet_count, "in_packet_count");
		add_stat(stats->inbound.media_packet_count, "in_media_packet_count");
		add_stat(stats->inbound.skip_packet_count, "in_skip_packet_count");
		add_stat(stats->inbound.jb_packet_count, "in_jb_packet_count");
		add_stat(stats->inbound.dtmf_packet_count, "in_dtmf_packet_count");
		add_stat(stats->inbound.cng_packet_count, "in_cng_packet_count");
		add_stat(stats->inbound.flush_packet_count, "in_flush_packet_count");
		add_stat(stats->inbound.largest_jb_size, "in_largest_jb_size");

		add_stat(stats->outbound.raw_bytes, "out_raw_bytes");
		add_stat(stats->outbound.media_bytes, "out_media_bytes");
		add_stat(stats->outbound.packet_count, "out_packet_count");
		add_stat(stats->outbound.media_packet_count, "out_media_packet_count");
		add_stat(stats->outbound.skip_packet_count, "out_skip_packet_count");
		add_stat(stats->outbound.dtmf_packet_count, "out_dtmf_packet_count");
		add_stat(stats->outbound.cng_packet_count, "out_cng_packet_count");

		add_stat(stats->rtcp.packet_count, "rtcp_packet_count");
		add_stat(stats->rtcp.octet_count, "rtcp_octet_count");

	}
}

SWITCH_DECLARE(void) switch_core_media_set_stats(switch_core_session_t *session)
{
	
	if (!session->media_handle) {
		return;
	}

	set_stats(session, SWITCH_MEDIA_TYPE_AUDIO, "audio");
	set_stats(session, SWITCH_MEDIA_TYPE_VIDEO, "video");
}



SWITCH_DECLARE(void) switch_media_handle_destroy(switch_core_session_t *session)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine, *v_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];	

	
	if (switch_core_codec_ready(&a_engine->read_codec)) {
		switch_core_codec_destroy(&a_engine->read_codec);
	}

	if (switch_core_codec_ready(&a_engine->write_codec)) {
		switch_core_codec_destroy(&a_engine->write_codec);
	}

	if (switch_core_codec_ready(&v_engine->read_codec)) {
		switch_core_codec_destroy(&v_engine->read_codec);
	}

	if (switch_core_codec_ready(&v_engine->write_codec)) {
		switch_core_codec_destroy(&v_engine->write_codec);
	}

	switch_core_session_unset_read_codec(session);
	switch_core_session_unset_write_codec(session);
	switch_core_media_deactivate_rtp(session);



}


SWITCH_DECLARE(switch_status_t) switch_media_handle_create(switch_media_handle_t **smhp, switch_core_session_t *session, switch_core_media_params_t *params)
{
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_media_handle_t *smh = NULL;


	*smhp = NULL;

	if ((session->media_handle = switch_core_session_alloc(session, (sizeof(*smh))))) {
		session->media_handle->session = session;
		*smhp = session->media_handle;
		switch_set_flag(session->media_handle, SMF_INIT);
		session->media_handle->media_flags[SCMF_RUNNING] = 1;
		session->media_handle->engines[SWITCH_MEDIA_TYPE_AUDIO].read_frame.buflen = SWITCH_RTP_MAX_BUF_LEN;
		session->media_handle->engines[SWITCH_MEDIA_TYPE_VIDEO].read_frame.buflen = SWITCH_RTP_MAX_BUF_LEN;
		session->media_handle->mparams = params;

		switch_mutex_init(&session->media_handle->mutex, SWITCH_MUTEX_NESTED, switch_core_session_get_pool(session));

		session->media_handle->engines[SWITCH_MEDIA_TYPE_AUDIO].ssrc = 
			(uint32_t) ((intptr_t) &session->media_handle->engines[SWITCH_MEDIA_TYPE_AUDIO] + (uint32_t) time(NULL));

		session->media_handle->engines[SWITCH_MEDIA_TYPE_VIDEO].ssrc = 
			(uint32_t) ((intptr_t) &session->media_handle->engines[SWITCH_MEDIA_TYPE_VIDEO] + (uint32_t) time(NULL) / 2);

		switch_channel_set_flag(session->channel, CF_DTLS_OK);

		status = SWITCH_STATUS_SUCCESS;
	}


	return status;
}

SWITCH_DECLARE(void) switch_media_handle_set_media_flag(switch_media_handle_t *smh, switch_core_media_flag_t flag)
{
	switch_assert(smh);

	smh->media_flags[flag] = 1;
	
}

SWITCH_DECLARE(void) switch_media_handle_set_media_flags(switch_media_handle_t *smh, switch_core_media_flag_t flags[SCMF_MAX])
{
	int i;
	switch_assert(smh);

	for(i = 0; i < SCMF_MAX; i++) {
		if (flags[i]) {
			smh->media_flags[i] = flags[i];
		}
	}
	
}

SWITCH_DECLARE(void) switch_media_handle_clear_media_flag(switch_media_handle_t *smh, switch_core_media_flag_t flag)
{
	switch_assert(smh);

	smh->media_flags[flag] = 0;
}

SWITCH_DECLARE(int32_t) switch_media_handle_test_media_flag(switch_media_handle_t *smh, switch_core_media_flag_t flag)
{
	switch_assert(smh);
	return smh->media_flags[flag];
}

SWITCH_DECLARE(switch_status_t) switch_core_session_media_handle_ready(switch_core_session_t *session)
{

	if (session->media_handle && switch_test_flag(session->media_handle, SMF_INIT)) {
		return SWITCH_STATUS_SUCCESS;
	}
	
	return SWITCH_STATUS_FALSE;
}


SWITCH_DECLARE(switch_media_handle_t *) switch_core_session_get_media_handle(switch_core_session_t *session)
{
	if (switch_core_session_media_handle_ready(session)) {
		return session->media_handle;
	}

	return NULL;
}

SWITCH_DECLARE(switch_status_t) switch_core_session_clear_media_handle(switch_core_session_t *session)
{
	if (!session->media_handle) {
		return SWITCH_STATUS_FALSE;
	}
	
	return SWITCH_STATUS_SUCCESS;
}


SWITCH_DECLARE(void) switch_core_media_prepare_codecs(switch_core_session_t *session, switch_bool_t force)
{
	const char *abs, *codec_string = NULL;
	const char *ocodec = NULL;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_channel_test_flag(session->channel, CF_PROXY_MODE) || switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
		return;
	}

	if (force) {
		smh->mparams->num_codecs = 0;
	}

	if (smh->mparams->num_codecs) {
		return;
	}

	smh->payload_space = 0;

	switch_assert(smh->session != NULL);

	if ((abs = switch_channel_get_variable(session->channel, "absolute_codec_string"))) {
		/* inherit_codec == true will implicitly clear the absolute_codec_string 
		   variable if used since it was the reason it was set in the first place and is no longer needed */
		if (switch_true(switch_channel_get_variable(session->channel, "inherit_codec"))) {
			switch_channel_set_variable(session->channel, "absolute_codec_string", NULL);
		}
		codec_string = abs;
		goto ready;
	}

	if (!(codec_string = switch_channel_get_variable(session->channel, "codec_string"))) {
		codec_string = switch_core_media_get_codec_string(smh->session);
	}

	if (codec_string && *codec_string == '=') {
		codec_string++;
		goto ready;
	}

	if ((ocodec = switch_channel_get_variable(session->channel, SWITCH_ORIGINATOR_CODEC_VARIABLE))) {
		if (!codec_string || (smh->media_flags[SCMF_DISABLE_TRANSCODING])) {
			codec_string = ocodec;
		} else {
			if (!(codec_string = switch_core_session_sprintf(smh->session, "%s,%s", ocodec, codec_string))) {
				codec_string = ocodec;
			}
		}
	}

 ready:
	if (codec_string) {
		char *tmp_codec_string = switch_core_session_strdup(smh->session, codec_string);
		switch_channel_set_variable(session->channel, "rtp_use_codec_string", codec_string);
		smh->codec_order_last = switch_separate_string(tmp_codec_string, ',', smh->codec_order, SWITCH_MAX_CODECS);
		smh->mparams->num_codecs = switch_loadable_module_get_codecs_sorted(smh->codecs, SWITCH_MAX_CODECS, smh->codec_order, smh->codec_order_last);
	} else {
		smh->mparams->num_codecs = switch_loadable_module_get_codecs(smh->codecs, sizeof(smh->codecs) / sizeof(smh->codecs[0]));
	}
}


//?
SWITCH_DECLARE(switch_status_t) switch_core_media_read_frame(switch_core_session_t *session, switch_frame_t **frame,
															 switch_io_flag_t flags, int stream_id, switch_media_type_t type)
{
	switch_rtcp_frame_t rtcp_frame;
	switch_rtp_engine_t *engine;
	switch_status_t status;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!smh->media_flags[SCMF_RUNNING]) {
		return SWITCH_STATUS_FALSE;
	}

	engine = &smh->engines[type];

	engine->read_frame.datalen = 0;

	if (!engine->read_codec.implementation || !switch_core_codec_ready(&engine->read_codec)) {
		return SWITCH_STATUS_FALSE;
	}

	switch_assert(engine->rtp_session != NULL);
	engine->read_frame.datalen = 0;

	
	while (smh->media_flags[SCMF_RUNNING] && engine->read_frame.datalen == 0) {
		engine->read_frame.flags = SFF_NONE;

		status = switch_rtp_zerocopy_read_frame(engine->rtp_session, &engine->read_frame, flags);

		if (status != SWITCH_STATUS_SUCCESS && status != SWITCH_STATUS_BREAK) {
			if (status == SWITCH_STATUS_TIMEOUT) {

				if (switch_channel_get_variable(session->channel, "execute_on_media_timeout")) {
					*frame = &engine->read_frame;
					switch_set_flag((*frame), SFF_CNG);
					(*frame)->datalen = engine->read_impl.encoded_bytes_per_packet;
					memset((*frame)->data, 0, (*frame)->datalen);
					switch_channel_execute_on(session->channel, "execute_on_media_timeout");
					return SWITCH_STATUS_SUCCESS;
				}


				switch_channel_hangup(session->channel, SWITCH_CAUSE_MEDIA_TIMEOUT);
			}
			return status;
		}

		/* Try to read an RTCP frame, if successful raise an event */
		if (switch_rtcp_zerocopy_read_frame(engine->rtp_session, &rtcp_frame) == SWITCH_STATUS_SUCCESS) {
			switch_event_t *event;

			if (switch_event_create(&event, SWITCH_EVENT_RECV_RTCP_MESSAGE) == SWITCH_STATUS_SUCCESS) {
				char value[30];
				char header[50];
				int i;

				char *uuid = switch_core_session_get_uuid(session);
				if (uuid) {
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Unique-ID", switch_core_session_get_uuid(session));
				}

				snprintf(value, sizeof(value), "%.8x", rtcp_frame.ssrc);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "SSRC", value);

				snprintf(value, sizeof(value), "%u", rtcp_frame.ntp_msw);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "NTP-Most-Significant-Word", value);

				snprintf(value, sizeof(value), "%u", rtcp_frame.ntp_lsw);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "NTP-Least-Significant-Word", value);

				snprintf(value, sizeof(value), "%u", rtcp_frame.timestamp);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "RTP-Timestamp", value);

				snprintf(value, sizeof(value), "%u", rtcp_frame.packet_count);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Sender-Packet-Count", value);

				snprintf(value, sizeof(value), "%u", rtcp_frame.octect_count);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Octect-Packet-Count", value);

				snprintf(value, sizeof(value), "%" SWITCH_SIZE_T_FMT, engine->read_frame.timestamp);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Last-RTP-Timestamp", value);

				snprintf(value, sizeof(value), "%u", engine->read_frame.rate);
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "RTP-Rate", value);

				snprintf(value, sizeof(value), "%" SWITCH_TIME_T_FMT, switch_time_now());
				switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, "Capture-Time", value);

				// Add sources info
				for (i = 0; i < rtcp_frame.report_count; i++) {
					snprintf(header, sizeof(header), "Source%u-SSRC", i);
					snprintf(value, sizeof(value), "%.8x", rtcp_frame.reports[i].ssrc);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
					snprintf(header, sizeof(header), "Source%u-Fraction", i);
					snprintf(value, sizeof(value), "%u", rtcp_frame.reports[i].fraction);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
					snprintf(header, sizeof(header), "Source%u-Lost", i);
					snprintf(value, sizeof(value), "%u", rtcp_frame.reports[i].lost);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
					snprintf(header, sizeof(header), "Source%u-Highest-Sequence-Number-Received", i);
					snprintf(value, sizeof(value), "%u", rtcp_frame.reports[i].highest_sequence_number_received);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
					snprintf(header, sizeof(header), "Source%u-Jitter", i);
					snprintf(value, sizeof(value), "%u", rtcp_frame.reports[i].jitter);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
					snprintf(header, sizeof(header), "Source%u-LSR", i);
					snprintf(value, sizeof(value), "%u", rtcp_frame.reports[i].lsr);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
					snprintf(header, sizeof(header), "Source%u-DLSR", i);
					snprintf(value, sizeof(value), "%u", rtcp_frame.reports[i].dlsr);
					switch_event_add_header_string(event, SWITCH_STACK_BOTTOM, header, value);
				}

				switch_event_fire(&event);
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG10, "Dispatched RTCP event\n");
			}
		}

		/* Fast PASS! */
		if (switch_test_flag((&engine->read_frame), SFF_PROXY_PACKET)) {
			*frame = &engine->read_frame;
			return SWITCH_STATUS_SUCCESS;
		}

		if (switch_rtp_has_dtmf(engine->rtp_session)) {
			switch_dtmf_t dtmf = { 0 };
			switch_rtp_dequeue_dtmf(engine->rtp_session, &dtmf);
			switch_channel_queue_dtmf(session->channel, &dtmf);
		}

		if (engine->read_frame.datalen > 0) {
			uint32_t bytes = 0;
			int frames = 1;

			if (!switch_test_flag((&engine->read_frame), SFF_CNG)) {
				if (!engine->read_codec.implementation || !switch_core_codec_ready(&engine->read_codec)) {
					*frame = NULL;
					return SWITCH_STATUS_GENERR;
				}

				if ((engine->read_frame.datalen % 10) == 0 &&
					(smh->media_flags[SCMF_AUTOFIX_TIMING]) && engine->check_frames < MAX_CODEC_CHECK_FRAMES) {
					engine->check_frames++;

					if (!engine->read_impl.encoded_bytes_per_packet) {
						engine->check_frames = MAX_CODEC_CHECK_FRAMES;
						goto skip;
					}

					if (engine->last_ts && engine->read_frame.datalen != engine->read_impl.encoded_bytes_per_packet) {
						uint32_t codec_ms = (int) (engine->read_frame.timestamp -
												   engine->last_ts) / (engine->read_impl.samples_per_second / 1000);

						if ((codec_ms % 10) != 0 || codec_ms > engine->read_impl.samples_per_packet * 10) {
							engine->last_ts = 0;
							goto skip;
						}


						if (engine->last_codec_ms && engine->last_codec_ms == codec_ms) {
							engine->mismatch_count++;
						}

						engine->last_codec_ms = codec_ms;

						if (engine->mismatch_count > MAX_MISMATCH_FRAMES) {
							if (switch_rtp_ready(engine->rtp_session) && codec_ms != engine->codec_ms) {
								const char *val;
								int rtp_timeout_sec = 0;
								int rtp_hold_timeout_sec = 0;

								if (codec_ms > 120) {	/* yeah right */
									switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
													  "Your phone is trying to send timestamps that suggest an increment of %dms per packet\n"
													  "That seems hard to believe so I am going to go on ahead and um ignore that, mmkay?\n",
													  (int) codec_ms);
									engine->check_frames = MAX_CODEC_CHECK_FRAMES;
									goto skip;
								}

								engine->read_frame.datalen = 0;

								if (codec_ms != engine->codec_ms) {
									switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
													  "Asynchronous PTIME not supported, changing our end from %d to %d\n",
													  (int) engine->codec_ms,
													  (int) codec_ms
													  );

									switch_channel_set_variable_printf(session->channel, "rtp_h_X-Broken-PTIME", "Adv=%d;Sent=%d",
																	   (int) engine->codec_ms, (int) codec_ms);

									engine->codec_ms = codec_ms;
								}


								if (switch_core_media_set_codec(session, 2, 0) != SWITCH_STATUS_SUCCESS) {
									*frame = NULL;
									return SWITCH_STATUS_GENERR;
								}

								if ((val = switch_channel_get_variable(session->channel, "rtp_timeout_sec"))) {
									int v = atoi(val);
									if (v >= 0) {
										rtp_timeout_sec = v;
									}
								}

								if ((val = switch_channel_get_variable(session->channel, "rtp_hold_timeout_sec"))) {
									int v = atoi(val);
									if (v >= 0) {
										rtp_hold_timeout_sec = v;
									}
								}

								if (rtp_timeout_sec) {
									engine->max_missed_packets = (engine->read_impl.samples_per_second * rtp_timeout_sec) /
										engine->read_impl.samples_per_packet;

									switch_rtp_set_max_missed_packets(engine->rtp_session, engine->max_missed_packets);
									if (!rtp_hold_timeout_sec) {
										rtp_hold_timeout_sec = rtp_timeout_sec * 10;
									}
								}

								if (rtp_hold_timeout_sec) {
									engine->max_missed_hold_packets = (engine->read_impl.samples_per_second * rtp_hold_timeout_sec) /
										engine->read_impl.samples_per_packet;
								}


								engine->check_frames = 0;
								engine->last_ts = 0;

								*frame = &engine->read_frame;
								switch_set_flag((*frame), SFF_CNG);
								(*frame)->datalen = engine->read_impl.encoded_bytes_per_packet;
								memset((*frame)->data, 0, (*frame)->datalen);
								return SWITCH_STATUS_SUCCESS;
							}

						}

					} else {
						engine->mismatch_count = 0;
					}

					engine->last_ts = engine->read_frame.timestamp;


				} else {
					engine->mismatch_count = 0;
					engine->last_ts = 0;
				}
			skip:

				if ((bytes = engine->read_impl.encoded_bytes_per_packet)) {
					frames = (engine->read_frame.datalen / bytes);
				}
				engine->read_frame.samples = (int) (frames * engine->read_impl.samples_per_packet);

				if (engine->read_frame.datalen == 0) {
					continue;
				}
			}
			break;
		}
	}
	
	if (engine->read_frame.datalen == 0) {
		*frame = NULL;
	}

	*frame = &engine->read_frame;

	return SWITCH_STATUS_SUCCESS;
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_write_frame(switch_core_session_t *session, 
												  switch_frame_t *frame, switch_io_flag_t flags, int stream_id, switch_media_type_t type)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	int bytes = 0, samples = 0, frames = 0;
	switch_rtp_engine_t *engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!smh->media_flags[SCMF_RUNNING]) {
		return SWITCH_STATUS_FALSE;
	}

	engine = &smh->engines[type];


	while (!(engine->read_codec.implementation && switch_rtp_ready(engine->rtp_session))) {
		if (switch_channel_ready(session->channel)) {
			switch_yield(10000);
		} else {
			return SWITCH_STATUS_GENERR;
		}
	}

	if (!engine->read_codec.implementation || !switch_core_codec_ready(&engine->read_codec)) {
		return SWITCH_STATUS_GENERR;
	}


	if (!engine->read_codec.implementation || !switch_core_codec_ready(&engine->read_codec)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!switch_test_flag(frame, SFF_CNG) && !switch_test_flag(frame, SFF_PROXY_PACKET)) {
		if (engine->read_impl.encoded_bytes_per_packet) {
			bytes = engine->read_impl.encoded_bytes_per_packet;
			frames = ((int) frame->datalen / bytes);
		} else
			frames = 1;

		samples = frames * engine->read_impl.samples_per_packet;
	}

	engine->timestamp_send += samples;

	if (!switch_rtp_write_frame(engine->rtp_session, frame)) {
		status = SWITCH_STATUS_FALSE;
	}


	return status;
}


//?
SWITCH_DECLARE(void) switch_core_media_copy_t38_options(switch_t38_options_t *t38_options, switch_core_session_t *session)
{
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_t38_options_t *local_t38_options = switch_channel_get_private(channel, "t38_options");

	switch_assert(t38_options);
	
	if (!local_t38_options) {
		local_t38_options = switch_core_session_alloc(session, sizeof(switch_t38_options_t));
	}

	local_t38_options->T38MaxBitRate = t38_options->T38MaxBitRate;
	local_t38_options->T38FaxFillBitRemoval = t38_options->T38FaxFillBitRemoval;
	local_t38_options->T38FaxTranscodingMMR = t38_options->T38FaxTranscodingMMR;
	local_t38_options->T38FaxTranscodingJBIG = t38_options->T38FaxTranscodingJBIG;
	local_t38_options->T38FaxRateManagement = switch_core_session_strdup(session, t38_options->T38FaxRateManagement);
	local_t38_options->T38FaxMaxBuffer = t38_options->T38FaxMaxBuffer;
	local_t38_options->T38FaxMaxDatagram = t38_options->T38FaxMaxDatagram;
	local_t38_options->T38FaxUdpEC = switch_core_session_strdup(session, t38_options->T38FaxUdpEC);
	local_t38_options->T38VendorInfo = switch_core_session_strdup(session, t38_options->T38VendorInfo);
	local_t38_options->remote_ip = switch_core_session_strdup(session, t38_options->remote_ip);
	local_t38_options->remote_port = t38_options->remote_port;


	switch_channel_set_private(channel, "t38_options", local_t38_options);

}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_get_offered_pt(switch_core_session_t *session, const switch_codec_implementation_t *mimp, switch_payload_t *pt)
{
	int i = 0;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}


	for (i = 0; i < smh->mparams->num_codecs; i++) {
		const switch_codec_implementation_t *imp = smh->codecs[i];

		if (!strcasecmp(imp->iananame, mimp->iananame) && imp->actual_samples_per_second == mimp->actual_samples_per_second) {
			*pt = smh->ianacodes[i];

			return SWITCH_STATUS_SUCCESS;
		}
	}

	return SWITCH_STATUS_FALSE;
}



//?
SWITCH_DECLARE(switch_status_t) switch_core_media_set_video_codec(switch_core_session_t *session, int force)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *v_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];


	if (!v_engine->codec_params.rm_encoding) {
		return SWITCH_STATUS_FALSE;
	}

	if (v_engine->read_codec.implementation && switch_core_codec_ready(&v_engine->read_codec)) {
		if (!force) {
			return SWITCH_STATUS_SUCCESS;
		}
		if (strcasecmp(v_engine->read_codec.implementation->iananame, v_engine->codec_params.rm_encoding) ||
			v_engine->read_codec.implementation->samples_per_second != v_engine->codec_params.rm_rate) {

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Changing Codec from %s to %s\n",
							  v_engine->read_codec.implementation->iananame, v_engine->codec_params.rm_encoding);
			switch_core_codec_destroy(&v_engine->read_codec);
			switch_core_codec_destroy(&v_engine->write_codec);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Already using %s\n",
							  v_engine->read_codec.implementation->iananame);
			return SWITCH_STATUS_SUCCESS;
		}
	}



	if (switch_core_codec_init(&v_engine->read_codec,
							   v_engine->codec_params.rm_encoding,
							   v_engine->codec_params.rm_fmtp,
							   v_engine->codec_params.rm_rate,
							   0,
							   1,
							   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
							   NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't load codec?\n");
		return SWITCH_STATUS_FALSE;
	} else {
		if (switch_core_codec_init(&v_engine->write_codec,
								   v_engine->codec_params.rm_encoding,
								   v_engine->codec_params.rm_fmtp,
								   v_engine->codec_params.rm_rate,
								   0,
								   1,
								   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE,
								   NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't load codec?\n");
			return SWITCH_STATUS_FALSE;
		} else {
			v_engine->read_frame.rate = v_engine->codec_params.rm_rate;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set VIDEO Codec %s %s/%ld %d ms\n",
							  switch_channel_get_name(session->channel), v_engine->codec_params.rm_encoding, 
							  v_engine->codec_params.rm_rate, v_engine->codec_params.codec_ms);
			v_engine->read_frame.codec = &v_engine->read_codec;

			v_engine->write_codec.fmtp_out = switch_core_session_strdup(session, v_engine->write_codec.fmtp_out);

			v_engine->write_codec.agreed_pt = v_engine->codec_params.agreed_pt;
			v_engine->read_codec.agreed_pt = v_engine->codec_params.agreed_pt;
			switch_core_session_set_video_read_codec(session, &v_engine->read_codec);
			switch_core_session_set_video_write_codec(session, &v_engine->write_codec);


			switch_channel_set_variable_printf(session->channel, "rtp_last_video_codec_string", "%s@%dh@%di", 
											   v_engine->codec_params.iananame, v_engine->codec_params.rm_rate, v_engine->codec_params.codec_ms);


			if (switch_rtp_ready(v_engine->rtp_session)) {
				switch_core_session_message_t msg = { 0 };

				msg.from = __FILE__;
				msg.message_id = SWITCH_MESSAGE_INDICATE_VIDEO_REFRESH_REQ;

				switch_rtp_set_default_payload(v_engine->rtp_session, v_engine->codec_params.agreed_pt);
				
				if (v_engine->codec_params.recv_pt != v_engine->codec_params.agreed_pt) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
									  "%s Set video receive payload to %u\n", switch_channel_get_name(session->channel), v_engine->codec_params.recv_pt);
					
					switch_rtp_set_recv_pt(v_engine->rtp_session, v_engine->codec_params.recv_pt);
				} else {
					switch_rtp_set_recv_pt(v_engine->rtp_session, v_engine->codec_params.agreed_pt);
				}

				switch_core_session_receive_message(session, &msg);


			}
			
			switch_channel_set_variable(session->channel, "rtp_use_video_codec_name", v_engine->codec_params.rm_encoding);
			switch_channel_set_variable(session->channel, "rtp_use_video_codec_fmtp", v_engine->codec_params.rm_fmtp);
			switch_channel_set_variable_printf(session->channel, "rtp_use_video_codec_rate", "%d", v_engine->codec_params.rm_rate);
			switch_channel_set_variable_printf(session->channel, "rtp_use_video_codec_ptime", "%d", 0);

		}
	}
	return SWITCH_STATUS_SUCCESS;
}


//?
SWITCH_DECLARE(switch_status_t) switch_core_media_set_codec(switch_core_session_t *session, int force, uint32_t codec_flags)
{
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	int resetting = 0;
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}
	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];

	if (!a_engine->codec_params.iananame) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "No audio codec available\n");
		switch_goto_status(SWITCH_STATUS_FALSE, end);
	}

	if (switch_core_codec_ready(&a_engine->read_codec)) {
		if (!force) {
			switch_goto_status(SWITCH_STATUS_SUCCESS, end);
		}
		if (strcasecmp(a_engine->read_impl.iananame, a_engine->codec_params.rm_encoding) ||
			(uint32_t) a_engine->read_impl.microseconds_per_packet / 1000 != a_engine->codec_params.codec_ms ||
			a_engine->read_impl.samples_per_second != a_engine->codec_params.rm_rate ) {
			

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
							  "Changing Codec from %s@%dms@%dhz to %s@%dms@%luhz\n",
							  a_engine->read_impl.iananame, 
							  a_engine->read_impl.microseconds_per_packet / 1000,
							  a_engine->read_impl.samples_per_second,

							  a_engine->codec_params.rm_encoding, 
							  a_engine->codec_params.codec_ms,
							  a_engine->codec_params.rm_rate);
			
			switch_yield(a_engine->read_impl.microseconds_per_packet);
			switch_core_session_lock_codec_write(session);
			switch_core_session_lock_codec_read(session);
			resetting = 1;
			switch_yield(a_engine->read_impl.microseconds_per_packet);
			switch_core_codec_destroy(&a_engine->read_codec);
			switch_core_codec_destroy(&a_engine->write_codec);
			switch_channel_audio_sync(session->channel);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Already using %s\n", a_engine->read_impl.iananame);
			switch_goto_status(SWITCH_STATUS_SUCCESS, end);
		}
	}

	if (switch_core_codec_init_with_bitrate(&a_engine->read_codec,
											a_engine->codec_params.iananame,
											a_engine->codec_params.rm_fmtp,
											a_engine->codec_params.rm_rate,
											a_engine->codec_params.codec_ms,
											a_engine->codec_params.channels,
											a_engine->codec_params.bitrate,
											SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE | codec_flags,
											NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't load codec?\n");
		switch_channel_hangup(session->channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		switch_goto_status(SWITCH_STATUS_FALSE, end);
	}
	
	a_engine->read_codec.session = session;


	if (switch_core_codec_init_with_bitrate(&a_engine->write_codec,
											a_engine->codec_params.iananame,
											a_engine->codec_params.rm_fmtp,
											a_engine->codec_params.rm_rate,
											a_engine->codec_params.codec_ms,
											a_engine->codec_params.channels,
											a_engine->codec_params.bitrate,
											SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE | codec_flags,
											NULL, switch_core_session_get_pool(session)) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't load codec?\n");
		switch_channel_hangup(session->channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
		switch_goto_status(SWITCH_STATUS_FALSE, end);
	}

	a_engine->write_codec.session = session;

	switch_channel_set_variable(session->channel, "rtp_use_codec_name", a_engine->codec_params.iananame);
	switch_channel_set_variable(session->channel, "rtp_use_codec_fmtp", a_engine->codec_params.rm_fmtp);
	switch_channel_set_variable_printf(session->channel, "rtp_use_codec_rate", "%d", a_engine->codec_params.rm_rate);
	switch_channel_set_variable_printf(session->channel, "rtp_use_codec_ptime", "%d", a_engine->codec_params.codec_ms);
	switch_channel_set_variable_printf(session->channel, "rtp_last_audio_codec_string", "%s@%dh@%di", 
									   a_engine->codec_params.iananame, a_engine->codec_params.rm_rate, a_engine->codec_params.codec_ms);

	switch_assert(a_engine->read_codec.implementation);
	switch_assert(a_engine->write_codec.implementation);

	a_engine->read_impl = *a_engine->read_codec.implementation;
	a_engine->write_impl = *a_engine->write_codec.implementation;

	switch_core_session_set_read_impl(session, a_engine->read_codec.implementation);
	switch_core_session_set_write_impl(session, a_engine->write_codec.implementation);

	if (switch_rtp_ready(a_engine->rtp_session)) {
		switch_assert(a_engine->read_codec.implementation);
		
		if (switch_rtp_change_interval(a_engine->rtp_session,
									   a_engine->read_impl.microseconds_per_packet, 
									   a_engine->read_impl.samples_per_packet) != SWITCH_STATUS_SUCCESS) {
			switch_channel_hangup(session->channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
			switch_goto_status(SWITCH_STATUS_FALSE, end);
		}
	}

	a_engine->read_frame.rate = a_engine->codec_params.rm_rate;

	if (!switch_core_codec_ready(&a_engine->read_codec)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Can't load codec?\n");
		switch_goto_status(SWITCH_STATUS_FALSE, end);
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set Codec %s %s/%ld %d ms %d samples %d bits\n",
					  switch_channel_get_name(session->channel), a_engine->codec_params.iananame, a_engine->codec_params.rm_rate, 
					  a_engine->codec_params.codec_ms,
					  a_engine->read_impl.samples_per_packet, a_engine->read_impl.bits_per_second);
	a_engine->read_frame.codec = &a_engine->read_codec;

	a_engine->write_codec.agreed_pt = a_engine->codec_params.agreed_pt;
	a_engine->read_codec.agreed_pt = a_engine->codec_params.agreed_pt;

	if (force != 2) {
		switch_core_session_set_real_read_codec(session, &a_engine->read_codec);
		switch_core_session_set_write_codec(session, &a_engine->write_codec);
	}

	a_engine->codec_params.fmtp_out = switch_core_session_strdup(session, a_engine->write_codec.fmtp_out);

	if (switch_rtp_ready(a_engine->rtp_session)) {
		switch_rtp_set_default_payload(a_engine->rtp_session, a_engine->codec_params.pt);
		switch_rtp_set_recv_pt(a_engine->rtp_session, a_engine->read_codec.agreed_pt);
	}

 end:
	if (resetting) {
		switch_core_session_unlock_codec_write(session);
		switch_core_session_unlock_codec_read(session);
	}

	switch_core_media_set_video_codec(session, force);

	return status;
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_add_ice_acl(switch_core_session_t *session, switch_media_type_t type, const char *acl_name)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	engine = &smh->engines[type];

	if (engine->cand_acl_count < SWITCH_MAX_CAND_ACL) {
		engine->cand_acl[engine->cand_acl_count++] = switch_core_session_strdup(session, acl_name);
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

//?
SWITCH_DECLARE(void) switch_core_media_check_video_codecs(switch_core_session_t *session)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (smh->mparams->num_codecs && !switch_channel_test_flag(session->channel, CF_VIDEO_POSSIBLE)) {
		int i;
		smh->video_count = 0;
		for (i = 0; i < smh->mparams->num_codecs; i++) {
			
			if (smh->codecs[i]->codec_type == SWITCH_CODEC_TYPE_VIDEO) {
				if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND &&
					switch_channel_test_flag(session->channel, CF_NOVIDEO)) {
					continue;
				}
				smh->video_count++;
			}
		}
		if (smh->video_count) {
			switch_channel_set_flag(session->channel, CF_VIDEO_POSSIBLE);
		}
	}
}

//?
static void generate_local_fingerprint(switch_media_handle_t *smh, switch_media_type_t type)
{
	switch_rtp_engine_t *engine = &smh->engines[type];

	if (!engine->local_dtls_fingerprint.len) {
		engine->local_dtls_fingerprint.type = "sha-256";
		switch_core_cert_gen_fingerprint(DTLS_SRTP_FNAME, &engine->local_dtls_fingerprint);
	}
}

//?
static int dtls_ok(switch_core_session_t *session)
{
	return switch_channel_test_flag(session->channel, CF_DTLS_OK);
}

#ifdef _MSC_VER
/* remove this if the break is removed from the following for loop which causes unreachable code loop */
/* for (i = 0; i < engine->cand_acl_count; i++) { */
#pragma warning(push)
#pragma warning(disable:4702)
#endif

//?
static void check_ice(switch_media_handle_t *smh, switch_media_type_t type, sdp_session_t *sdp, sdp_media_t *m)
{
	switch_rtp_engine_t *engine = &smh->engines[type];
	sdp_attribute_t *attr;
	int i = 0, got_rtcp_mux = 0;

	if (engine->ice_in.chosen[0] && engine->ice_in.chosen[1] && !switch_channel_test_flag(smh->session->channel, CF_REINVITE)) {
		return;
	}

	engine->ice_in.chosen[0] = 0;
	engine->ice_in.chosen[1] = 0;
	engine->ice_in.cand_idx = 0;

	if (m) {
		attr = m->m_attributes;
	} else {
		attr = sdp->sdp_attributes;
	}

	for (; attr; attr = attr->a_next) {
		char *data;
		char *fields[15];
		int argc = 0, j = 0;
		int cid = 0;

		if (zstr(attr->a_name)) {
			continue;
		}

		if (!strcasecmp(attr->a_name, "ice-ufrag")) {
			engine->ice_in.ufrag = switch_core_session_strdup(smh->session, attr->a_value);
		} else if (!strcasecmp(attr->a_name, "ice-pwd")) {
			engine->ice_in.pwd = switch_core_session_strdup(smh->session, attr->a_value);
		} else if (!strcasecmp(attr->a_name, "ice-options")) {
			engine->ice_in.options = switch_core_session_strdup(smh->session, attr->a_value);
			
		} else if (switch_rtp_has_dtls() && dtls_ok(smh->session) && !strcasecmp(attr->a_name, "fingerprint") && !zstr(attr->a_value)) {
			char *p;

			engine->remote_dtls_fingerprint.type = switch_core_session_strdup(smh->session, attr->a_value);
			
			if ((p = strchr(engine->remote_dtls_fingerprint.type, ' '))) {
				*p++ = '\0';
				switch_set_string(engine->local_dtls_fingerprint.str, p);
			}
			
			if (strcasecmp(engine->remote_dtls_fingerprint.type, "sha-256")) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_WARNING, "Unsupported fingerprint type.\n");
				engine->local_dtls_fingerprint.type = NULL;
				engine->remote_dtls_fingerprint.type = NULL;
			}


			generate_local_fingerprint(smh, type);
			switch_channel_set_flag(smh->session->channel, CF_DTLS);

		} else if (!engine->remote_ssrc && !strcasecmp(attr->a_name, "ssrc") && attr->a_value) {
			engine->remote_ssrc = (uint32_t) atol(attr->a_value);
#ifdef RTCP_MUX
		} else if (!strcasecmp(attr->a_name, "rtcp-mux")) {
			engine->rtcp_mux = SWITCH_TRUE;
			engine->remote_rtcp_port = engine->codec_params.remote_sdp_port;
			got_rtcp_mux++;
#endif
		} else if (!strcasecmp(attr->a_name, "candidate")) {
			switch_channel_set_flag(smh->session->channel, CF_ICE);

			if (!engine->cand_acl_count) {
				engine->cand_acl[engine->cand_acl_count++] = "wan.auto";
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_WARNING, "NO candidate ACL defined, Defaulting to wan.auto\n");
			}


			if (!switch_stristr(" udp ", attr->a_value)) {
				continue;
			}

			data = switch_core_session_strdup(smh->session, attr->a_value);

			argc = switch_split(data, ' ', fields);
			
			if (argc < 5 || engine->ice_in.cand_idx >= MAX_CAND) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_WARNING, "Invalid data\n");
				continue;
			}

			cid = atoi(fields[1]) - 1;


			for (i = 0; i < argc; i++) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_DEBUG1, "CAND %d [%s]\n", i, fields[i]);
			}

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_DEBUG, 
							  "Checking Candidate cid: %d proto: %s type: %s addr: %s:%s\n", cid+1, fields[2], fields[7], fields[4], fields[5]);


			engine->ice_in.cand_idx++;

			for (i = 0; i < engine->cand_acl_count; i++) {
				if (!engine->ice_in.chosen[cid] && switch_check_network_list_ip(fields[4], engine->cand_acl[i])) {
					engine->ice_in.chosen[cid] = engine->ice_in.cand_idx;
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
									  "Choose %s Candidate cid: %d proto: %s type: %s addr: %s:%s\n", 
									  type == SWITCH_MEDIA_TYPE_VIDEO ? "video" : "audio",
									  cid+1, fields[2], fields[7], fields[4], fields[5]);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
									  "Save %s Candidate cid: %d proto: %s type: %s addr: %s:%s\n", 
									  type == SWITCH_MEDIA_TYPE_VIDEO ? "video" : "audio",
									  cid+1, fields[2], fields[7], fields[4], fields[5]);
				}

				engine->ice_in.cands[engine->ice_in.cand_idx][cid].foundation = switch_core_session_strdup(smh->session, fields[0]);
				engine->ice_in.cands[engine->ice_in.cand_idx][cid].component_id = atoi(fields[1]);
				engine->ice_in.cands[engine->ice_in.cand_idx][cid].transport = switch_core_session_strdup(smh->session, fields[2]);
				engine->ice_in.cands[engine->ice_in.cand_idx][cid].priority = atol(fields[3]);
				engine->ice_in.cands[engine->ice_in.cand_idx][cid].con_addr = switch_core_session_strdup(smh->session, fields[4]);
				engine->ice_in.cands[engine->ice_in.cand_idx][cid].con_port = (switch_port_t)atoi(fields[5]);
						
				j = 6;

				while(j < argc && fields[j+1]) {
					if (!strcasecmp(fields[j], "typ")) {
						engine->ice_in.cands[engine->ice_in.cand_idx][cid].cand_type = switch_core_session_strdup(smh->session, fields[j+1]);							
					} else if (!strcasecmp(fields[j], "raddr")) {
						engine->ice_in.cands[engine->ice_in.cand_idx][cid].raddr = switch_core_session_strdup(smh->session, fields[j+1]);
					} else if (!strcasecmp(fields[j], "rport")) {
						engine->ice_in.cands[engine->ice_in.cand_idx][cid].rport = (switch_port_t)atoi(fields[j+1]);
					} else if (!strcasecmp(fields[j], "generation")) {
						engine->ice_in.cands[engine->ice_in.cand_idx][cid].generation = switch_core_session_strdup(smh->session, fields[j+1]);
					}
					
					j += 2;
				} 

				if (engine->ice_in.chosen[cid]) {
					engine->ice_in.cands[engine->ice_in.chosen[cid]][cid].ready++;
				}
				
				break;
			}
		}
		
	}
	
	/* still no candidates, so start searching for some based on sane deduction */

	/* look for candidates on the same network */
	if (!engine->ice_in.chosen[0] || !engine->ice_in.chosen[1]) {
		for (i = 0; i <= engine->ice_in.cand_idx && (!engine->ice_in.chosen[0] || !engine->ice_in.chosen[1]); i++) {
			if (!engine->ice_in.chosen[0] && engine->ice_in.cands[i][0].component_id == 1 && 
				!engine->ice_in.cands[i][0].rport && switch_check_network_list_ip(engine->ice_in.cands[i][0].con_addr, "localnet.auto")) {
				engine->ice_in.chosen[0] = i;
				engine->ice_in.cands[engine->ice_in.chosen[0]][0].ready++;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
								  "No %s RTP candidate found; defaulting to the first local one.\n", type2str(type));
			}
			if (!engine->ice_in.chosen[1] && engine->ice_in.cands[i][1].component_id == 2 && 
				!engine->ice_in.cands[i][1].rport && switch_check_network_list_ip(engine->ice_in.cands[i][1].con_addr, "localnet.auto")) {
				engine->ice_in.chosen[1] = i;
				engine->ice_in.cands[engine->ice_in.chosen[1]][1].ready++;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session),SWITCH_LOG_NOTICE, 
								  "No %s RTCP candidate found; defaulting to the first local one.\n", type2str(type));
			}
		}
	}

	/* look for candidates with srflx */
	if (!engine->ice_in.chosen[0] || !engine->ice_in.chosen[1]) {
		for (i = 0; i <= engine->ice_in.cand_idx && (!engine->ice_in.chosen[0] || !engine->ice_in.chosen[1]); i++) {
			if (!engine->ice_in.chosen[0] && engine->ice_in.cands[i][0].component_id == 1 && engine->ice_in.cands[i][0].rport) {
				engine->ice_in.chosen[0] = i;
				engine->ice_in.cands[engine->ice_in.chosen[0]][0].ready++;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
								  "No %s RTP candidate found; defaulting to the first srflx one.\n", type2str(type));
			}
			if (!engine->ice_in.chosen[1] && engine->ice_in.cands[i][1].component_id == 2 && engine->ice_in.cands[i][1].rport) {
				engine->ice_in.chosen[1] = i;
				engine->ice_in.cands[engine->ice_in.chosen[1]][1].ready++;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session),SWITCH_LOG_NOTICE, 
								  "No %s RTCP candidate found; defaulting to the first srflx one.\n", type2str(type));
			}
		}
	}

	/* look for any candidates and hope for auto-adjust */
	if (!engine->ice_in.chosen[0] || !engine->ice_in.chosen[1]) {
		for (i = 0; i <= engine->ice_in.cand_idx && (!engine->ice_in.chosen[0] || !engine->ice_in.chosen[1]); i++) {
			if (!engine->ice_in.chosen[0] && engine->ice_in.cands[i][0].component_id == 1) {
				engine->ice_in.chosen[0] = i;
				engine->ice_in.cands[engine->ice_in.chosen[0]][0].ready++;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
								  "No %s RTP candidate found; defaulting to the first one.\n", type2str(type));
			}
			if (!engine->ice_in.chosen[1] && engine->ice_in.cands[i][1].component_id == 2) {
				engine->ice_in.chosen[1] = i;
				engine->ice_in.cands[engine->ice_in.chosen[1]][1].ready++;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
								  "No %s RTCP candidate found; defaulting to the first one.\n", type2str(type));
			}
		}
	}

	for (i = 0; i < 2; i++) {
		if (engine->ice_in.cands[engine->ice_in.chosen[i]][i].ready) {
			if (zstr(engine->ice_in.ufrag) || zstr(engine->ice_in.pwd)) {
				engine->ice_in.cands[engine->ice_in.chosen[i]][i].ready = 0;
			}
		}
	}


	if (engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_addr && engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_port) {
		char tmp[80] = "";
		engine->codec_params.remote_sdp_ip = switch_core_session_strdup(smh->session, (char *) engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_addr);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE, 
						  "setting remote %s ice addr to %s:%d based on candidate\n", type2str(type),
						  engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_addr, engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_port);
		engine->ice_in.cands[engine->ice_in.chosen[0]][0].ready++;

		engine->remote_rtp_ice_port = (switch_port_t) engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_port;
		engine->remote_rtp_ice_addr = switch_core_session_strdup(smh->session, engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_addr);

		engine->codec_params.remote_sdp_ip = switch_core_session_strdup(smh->session, (char *) engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_addr);
		engine->codec_params.remote_sdp_port = (switch_port_t) engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_port;

																 
		switch_snprintf(tmp, sizeof(tmp), "%d", engine->codec_params.remote_sdp_port);
		switch_channel_set_variable(smh->session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE, engine->codec_params.remote_sdp_ip);
		switch_channel_set_variable(smh->session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE, tmp);					
	}

	if (engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_port) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_NOTICE,
						  "setting remote rtcp %s addr to %s:%d based on candidate\n", type2str(type),
						  engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_addr, engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_port);
		engine->remote_rtcp_ice_port = engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_port;
		engine->remote_rtcp_ice_addr = switch_core_session_strdup(smh->session, engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_addr);

		engine->remote_rtcp_port = engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_port;
	}


	if (m && !got_rtcp_mux) {
		engine->rtcp_mux = -1;
	}


	
	if (switch_channel_test_flag(smh->session->channel, CF_REINVITE)) {
		if (switch_rtp_ready(engine->rtp_session) && engine->ice_in.cands[engine->ice_in.chosen[0]][0].ready) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_INFO, "RE-Activating %s ICE\n", type2str(type));

			switch_rtp_activate_ice(engine->rtp_session, 
									engine->ice_in.ufrag,
									engine->ice_out.ufrag,
									engine->ice_out.pwd,
									engine->ice_in.pwd,
									IPR_RTP,
#ifdef GOOGLE_ICE
									ICE_GOOGLE_JINGLE,
									NULL
#else
									switch_channel_direction(smh->session->channel) == 
									SWITCH_CALL_DIRECTION_OUTBOUND ? ICE_VANILLA : (ICE_VANILLA | ICE_CONTROLLED),
									&engine->ice_in
#endif
									);

		
			
		}
		


		if (engine->ice_in.cands[engine->ice_in.chosen[1]][1].ready) {
			if (!strcmp(engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_addr, engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_addr)
				&& engine->ice_in.cands[engine->ice_in.chosen[1]][1].con_port == engine->ice_in.cands[engine->ice_in.chosen[0]][0].con_port) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_INFO, "Skipping %s RTCP ICE (Same as RTP)\n", type2str(type));
			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(smh->session), SWITCH_LOG_INFO, "Activating %s RTCP ICE\n", type2str(type));
				
				switch_rtp_activate_ice(engine->rtp_session, 
										engine->ice_in.ufrag,
										engine->ice_out.ufrag,
										engine->ice_out.pwd,
										engine->ice_in.pwd,
										IPR_RTCP,
#ifdef GOOGLE_ICE
										ICE_GOOGLE_JINGLE,
										NULL
#else
										switch_channel_direction(smh->session->channel) == 
										SWITCH_CALL_DIRECTION_OUTBOUND ? ICE_VANILLA : (ICE_VANILLA | ICE_CONTROLLED),
										&engine->ice_in
#endif
										);
			}
			
		}
		
	}
	
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

SWITCH_DECLARE(void) switch_core_session_set_ice(switch_core_session_t *session)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	switch_channel_set_flag(session->channel, CF_VERBOSE_SDP);
	switch_channel_set_flag(session->channel, CF_WEBRTC);
	switch_channel_set_flag(session->channel, CF_ICE);
	smh->mparams->rtcp_audio_interval_msec = "10000";
	smh->mparams->rtcp_video_interval_msec = "10000";
}

//?
SWITCH_DECLARE(uint8_t) switch_core_media_negotiate_sdp(switch_core_session_t *session, const char *r_sdp, uint8_t *proceed, switch_sdp_type_t sdp_type)
{
	uint8_t match = 0;
	switch_payload_t best_te = 0, te = 0, cng_pt = 0;
	sdp_media_t *m;
	sdp_attribute_t *attr;
	int first = 0, last = 0;
	int ptime = 0, dptime = 0, maxptime = 0, dmaxptime = 0;
	int sendonly = 0, recvonly = 0;
	int greedy = 0, x = 0, skip = 0, mine = 0;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *val;
	const char *crypto = NULL;
	int got_crypto = 0, got_video_crypto = 0, got_audio = 0, got_avp = 0, got_video_avp = 0, got_video_savp = 0, got_savp = 0, got_udptl = 0, got_webrtc = 0;
	int scrooge = 0;
	sdp_parser_t *parser = NULL;
	sdp_session_t *sdp;
	int reneg = 1;
	const switch_codec_implementation_t **codec_array;
	int total_codecs;
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;
	uint32_t near_rate = 0;
	const switch_codec_implementation_t *mimp = NULL, *near_match = NULL;
	sdp_rtpmap_t *mmap = NULL, *near_map = NULL;
	int codec_ms = 0;
	const char *tmp;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return 0;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	codec_array = smh->codecs;
	total_codecs = smh->mparams->num_codecs;

	if (!(parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
		return 0;
	}

	if (!(sdp = sdp_session(parser))) {
		sdp_parser_free(parser);
		return 0;
	}

	if (dtls_ok(session) && (tmp = switch_channel_get_variable(smh->session->channel, "webrtc_enable_dtls")) && switch_false(tmp)) {
		switch_channel_clear_flag(smh->session->channel, CF_DTLS_OK);
		switch_channel_clear_flag(smh->session->channel, CF_DTLS);
	}

	if (proceed) *proceed = 1;

	greedy = !!switch_media_handle_test_media_flag(smh, SCMF_CODEC_GREEDY);
	scrooge = !!switch_media_handle_test_media_flag(smh, SCMF_CODEC_SCROOGE);

	if ((val = switch_channel_get_variable(channel, "rtp_codec_negotiation"))) {
		if (!strcasecmp(val, "generous")) {
			greedy = 0;
			scrooge = 0;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "rtp_codec_negotiation overriding sofia inbound-codec-negotiation : generous\n" );
		} else if (!strcasecmp(val, "greedy")) {
			greedy = 1;
			scrooge = 0;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "rtp_codec_negotiation overriding sofia inbound-codec-negotiation : greedy\n" );
		} else if (!strcasecmp(val, "scrooge")) {
			scrooge = 1;
			greedy = 1;
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "rtp_codec_negotiation overriding sofia inbound-codec-negotiation : scrooge\n" );
		} else {
		    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "rtp_codec_negotiation ignored invalid value : '%s' \n", val );	
		}		
	}

	if ((smh->origin = switch_core_session_strdup(session, (char *) sdp->sdp_origin->o_username))) {

		if ((smh->mparams->auto_rtp_bugs & RTP_BUG_CISCO_SKIP_MARK_BIT_2833)) {

			if (strstr(smh->origin, "CiscoSystemsSIP-GW-UserAgent")) {
				a_engine->rtp_bugs |= RTP_BUG_CISCO_SKIP_MARK_BIT_2833;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Activate Buggy RFC2833 Mode!\n");
			}
		}

		if ((smh->mparams->auto_rtp_bugs & RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833)) {
			if (strstr(smh->origin, "Sonus_UAC")) {
				a_engine->rtp_bugs |= RTP_BUG_SONUS_SEND_INVALID_TIMESTAMP_2833;
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
								  "Hello,\nI see you have a Sonus!\n"
								  "FYI, Sonus cannot follow the RFC on the proper way to send DTMF.\n"
								  "Sadly, my creator had to spend several hours figuring this out so I thought you'd like to know that!\n"
								  "Don't worry, DTMF will work but you may want to ask them to fix it......\n");
			}
		}
	}

	if ((val = switch_channel_get_variable(session->channel, "rtp_liberal_dtmf")) && switch_true(val)) {
		switch_channel_set_flag(session->channel, CF_LIBERAL_DTMF);
	}

	if ((m = sdp->sdp_media) && 
		(m->m_mode == sdp_sendonly || m->m_mode == sdp_inactive || 
		 (m->m_connections && m->m_connections->c_address && !strcmp(m->m_connections->c_address, "0.0.0.0")))) {
		sendonly = 2;			/* global sendonly always wins */
	}

	for (attr = sdp->sdp_attributes; attr; attr = attr->a_next) {
		if (zstr(attr->a_name)) {
			continue;
		}

		if (!strcasecmp(attr->a_name, "sendonly")) {
			sendonly = 1;
			switch_channel_set_variable(session->channel, "media_audio_mode", "recvonly");
		} else if (!strcasecmp(attr->a_name, "inactive")) {
			sendonly = 1;
			switch_channel_set_variable(session->channel, "media_audio_mode", "inactive");
		} else if (!strcasecmp(attr->a_name, "recvonly")) {
			switch_channel_set_variable(session->channel, "media_audio_mode", "sendonly");
			recvonly = 1;

			if (switch_rtp_ready(a_engine->rtp_session)) {
				switch_rtp_set_max_missed_packets(a_engine->rtp_session, 0);
				a_engine->max_missed_hold_packets = 0;
				a_engine->max_missed_packets = 0;
			} else {
				switch_channel_set_variable(session->channel, "rtp_timeout_sec", "0");
				switch_channel_set_variable(session->channel, "rtp_hold_timeout_sec", "0");
			}
		} else if (sendonly < 2 && !strcasecmp(attr->a_name, "sendrecv")) {
			sendonly = 0;
		} else if (!strcasecmp(attr->a_name, "ptime")) {
			dptime = atoi(attr->a_value);
		} else if (!strcasecmp(attr->a_name, "maxptime")) {
			dmaxptime = atoi(attr->a_value);
		}
	}

	if (sendonly != 1 && recvonly != 1) {
		switch_channel_set_variable(session->channel, "media_audio_mode", NULL);
	}


	if (switch_media_handle_test_media_flag(smh, SCMF_DISABLE_HOLD) ||
		((val = switch_channel_get_variable(session->channel, "rtp_disable_hold")) && switch_true(val))) {
		sendonly = 0;
	} else {

		if (!smh->mparams->hold_laps) {
			smh->mparams->hold_laps++;
			if (switch_core_media_toggle_hold(session, sendonly)) {
				reneg = switch_media_handle_test_media_flag(smh, SCMF_RENEG_ON_HOLD);
				
				if ((val = switch_channel_get_variable(session->channel, "rtp_renegotiate_codec_on_hold"))) {
					reneg = switch_true(val);
				}
			}
			
		}
	}

	if (reneg) {
		reneg = switch_media_handle_test_media_flag(smh, SCMF_RENEG_ON_REINVITE);
		
		if ((val = switch_channel_get_variable(session->channel, "rtp_renegotiate_codec_on_reinvite"))) {
			reneg = switch_true(val);
		}
	}

	if (!reneg && smh->num_negotiated_codecs) {
		codec_array = smh->negotiated_codecs;
		total_codecs = smh->num_negotiated_codecs;
	} else if (reneg) {
		smh->mparams->num_codecs = 0;
		switch_core_media_prepare_codecs(session, SWITCH_FALSE);
		codec_array = smh->codecs;
		total_codecs = smh->mparams->num_codecs;
	}

	if (switch_stristr("T38FaxFillBitRemoval:", r_sdp) || switch_stristr("T38FaxTranscodingMMR:", r_sdp) || 
		switch_stristr("T38FaxTranscodingJBIG:", r_sdp)) {
		switch_channel_set_variable(session->channel, "t38_broken_boolean", "true");
	}

	switch_core_media_find_zrtp_hash(session, sdp);
	switch_core_media_pass_zrtp_hash(session);

	check_ice(smh, SWITCH_MEDIA_TYPE_AUDIO, sdp, NULL);
	check_ice(smh, SWITCH_MEDIA_TYPE_VIDEO, sdp, NULL);

	for (m = sdp->sdp_media; m; m = m->m_next) {
		sdp_connection_t *connection;
		switch_core_session_t *other_session;

		ptime = dptime;
		maxptime = dmaxptime;

		if (m->m_proto == sdp_proto_extended_srtp) {
			got_webrtc++;
			switch_core_session_set_ice(session);
		}
		
		if (m->m_proto_name && !strcasecmp(m->m_proto_name, "UDP/TLS/RTP/SAVPF")) {
			switch_channel_set_flag(session->channel, CF_WEBRTC_MOZ);
		}

		if (m->m_proto == sdp_proto_srtp || m->m_proto == sdp_proto_extended_srtp) {
			if (m->m_type == sdp_media_audio) {
				got_savp++;
			} else {
				got_video_savp++;
			}
		} else if (m->m_proto == sdp_proto_rtp) {
			if (m->m_type == sdp_media_audio) {
				got_avp++;
			} else {
				got_video_avp++;
			}
		} else if (m->m_proto == sdp_proto_udptl) {
			got_udptl++;
		}

		if (got_udptl && m->m_type == sdp_media_image && m->m_port) {
			switch_t38_options_t *t38_options = switch_core_media_process_udptl(session, sdp, m);

			if (switch_channel_test_app_flag_key("T38", session->channel, CF_APP_T38_NEGOTIATED)) {
				match = 1;
				goto done;
			}

			if (switch_true(switch_channel_get_variable(channel, "refuse_t38"))) {
				switch_channel_clear_app_flag_key("T38", session->channel, CF_APP_T38);
				match = 0;
				goto done;
			} else {
				const char *var = switch_channel_get_variable(channel, "t38_passthru");
				int pass = switch_channel_test_flag(smh->session->channel, CF_T38_PASSTHRU);


				if (switch_channel_test_app_flag_key("T38", session->channel, CF_APP_T38)) {
					if (proceed) *proceed = 0;
				}

				if (var) {
					if (!(pass = switch_true(var))) {
						if (!strcasecmp(var, "once")) {
							pass = 2;
						}
					}
				}

				if ((pass == 2 && switch_channel_test_flag(smh->session->channel, CF_T38_PASSTHRU)) 
					|| !switch_channel_test_flag(session->channel, CF_REINVITE) ||
					
					switch_channel_test_flag(session->channel, CF_PROXY_MODE) || 
					switch_channel_test_flag(session->channel, CF_PROXY_MEDIA) || 
					!switch_rtp_ready(a_engine->rtp_session)) {
					pass = 0;
				}
				
				if (pass && switch_core_session_get_partner(session, &other_session) == SWITCH_STATUS_SUCCESS) {
					switch_channel_t *other_channel = switch_core_session_get_channel(other_session);
					switch_core_session_message_t *msg;
					char *remote_host = switch_rtp_get_remote_host(a_engine->rtp_session);
					switch_port_t remote_port = switch_rtp_get_remote_port(a_engine->rtp_session);
					char tmp[32] = "";


                    if (!switch_channel_test_flag(other_channel, CF_ANSWERED)) {
                        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                                          SWITCH_LOG_WARNING, "%s Error Passing T.38 to unanswered channel %s\n",
                                          switch_channel_get_name(session->channel), switch_channel_get_name(other_channel));
                        switch_core_session_rwunlock(other_session);
                        //sofia_set_flag(session, TFLAG_NOREPLY);
                        pass = 0;
                        match = 0;
                        goto done;
                    }


					if (switch_true(switch_channel_get_variable(session->channel, "t38_broken_boolean")) && 
						switch_true(switch_channel_get_variable(session->channel, "t38_pass_broken_boolean"))) {
						switch_channel_set_variable(other_channel, "t38_broken_boolean", "true");
					}
					
					a_engine->codec_params.remote_sdp_ip = switch_core_session_strdup(session, t38_options->remote_ip);
					a_engine->codec_params.remote_sdp_port = t38_options->remote_port;

					if (remote_host && remote_port && !strcmp(remote_host, a_engine->codec_params.remote_sdp_ip) && remote_port == a_engine->codec_params.remote_sdp_port) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio params are unchanged for %s.\n",
										  switch_channel_get_name(session->channel));
					} else {
						const char *err = NULL;

						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio params changed for %s from %s:%d to %s:%d\n",
										  switch_channel_get_name(session->channel),
										  remote_host, remote_port, a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);
						
						switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.remote_sdp_port);
						switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE, a_engine->codec_params.remote_sdp_ip);
						switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE, tmp);

						if (switch_rtp_set_remote_address(a_engine->rtp_session, a_engine->codec_params.remote_sdp_ip,
														  a_engine->codec_params.remote_sdp_port, 0, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AUDIO RTP REPORTS ERROR: [%s]\n", err);
							switch_channel_hangup(channel, SWITCH_CAUSE_INCOMPATIBLE_DESTINATION);
						}
						
					}

					

					switch_core_media_copy_t38_options(t38_options, other_session);

					switch_channel_set_flag(smh->session->channel, CF_T38_PASSTHRU);
					switch_channel_set_flag(other_session->channel, CF_T38_PASSTHRU);
					
					msg = switch_core_session_alloc(other_session, sizeof(*msg));
					msg->message_id = SWITCH_MESSAGE_INDICATE_REQUEST_IMAGE_MEDIA;
					msg->from = __FILE__;
					msg->string_arg = switch_core_session_strdup(other_session, r_sdp);
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Passing T38 req to other leg.\n%s\n", r_sdp);
					switch_core_session_queue_message(other_session, msg);
					switch_core_session_rwunlock(other_session);
				}
			}


			/* do nothing here, mod_fax will trigger a response (if it's listening =/) */
			match = 1;
			goto done;
		} else if (m->m_type == sdp_media_audio && m->m_port && !got_audio) {
			sdp_rtpmap_t *map;

			if (switch_rtp_has_dtls() && dtls_ok(session)) {
				for (attr = m->m_attributes; attr; attr = attr->a_next) {
					
					if (!strcasecmp(attr->a_name, "fingerprint") && !zstr(attr->a_value)) {
						got_crypto = 1;
					}
				}
			}

			for (attr = m->m_attributes; attr; attr = attr->a_next) {

				if (!strcasecmp(attr->a_name, "rtcp") && attr->a_value) {
					switch_channel_set_variable(session->channel, "rtp_remote_audio_rtcp_port", attr->a_value);
					a_engine->remote_rtcp_port = (switch_port_t)atoi(attr->a_value);
				} else if (!strcasecmp(attr->a_name, "ptime") && attr->a_value) {
					ptime = atoi(attr->a_value);
				} else if (!strcasecmp(attr->a_name, "maxptime") && attr->a_value) {
					maxptime = atoi(attr->a_value);
				} else if (!got_crypto && !strcasecmp(attr->a_name, "crypto") && !zstr(attr->a_value) && 
						   (!switch_channel_test_flag(session->channel, CF_WEBRTC) || switch_stristr(SWITCH_RTP_CRYPTO_KEY_80, attr->a_value))) {
					int crypto_tag;

					if (!(smh->mparams->ndlb & SM_NDLB_ALLOW_CRYPTO_IN_AVP) && 
						!switch_true(switch_channel_get_variable(session->channel, "rtp_allow_crypto_in_avp"))) {
						if (m->m_proto != sdp_proto_srtp && !got_webrtc) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "a=crypto in RTP/AVP, refer to rfc3711\n");
							match = 0;
							goto done;
						}
					}

					crypto = attr->a_value;
					crypto_tag = atoi(crypto);

					got_crypto = switch_core_session_check_incoming_crypto(session, 
																		   "rtp_has_crypto", SWITCH_MEDIA_TYPE_AUDIO, crypto, crypto_tag, sdp_type);

				}
			}

			if (got_crypto && !got_avp) {
				switch_channel_set_variable(session->channel, "rtp_crypto_mandatory", "true");
				switch_channel_set_variable(session->channel, "rtp_secure_media", "true");
			}

			connection = sdp->sdp_connection;
			if (m->m_connections) {
				connection = m->m_connections;
			}

			if (!connection) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot find a c= line in the sdp at media or session level!\n");
				match = 0;
				break;
			}

		greed:
			x = 0;
			
			if (a_engine->codec_params.rm_encoding && !(switch_media_handle_test_media_flag(smh, SCMF_LIBERAL_DTMF) || 
														switch_channel_test_flag(session->channel, CF_LIBERAL_DTMF))) {	// && !switch_channel_test_flag(session->channel, CF_REINVITE)) {
				char *remote_host = a_engine->codec_params.remote_sdp_ip;
				switch_port_t remote_port = a_engine->codec_params.remote_sdp_port;
				int same = 0;

				if (switch_rtp_ready(a_engine->rtp_session)) {
					remote_host = switch_rtp_get_remote_host(a_engine->rtp_session);
					remote_port = switch_rtp_get_remote_port(a_engine->rtp_session);
				}

				for (map = m->m_rtpmaps; map; map = map->rm_next) {
					if ((zstr(map->rm_encoding) || (smh->mparams->ndlb & SM_NDLB_ALLOW_BAD_IANANAME)) && map->rm_pt < 96) {
						match = (map->rm_pt == a_engine->codec_params.pt) ? 1 : 0;
					} else {
						match = strcasecmp(switch_str_nil(map->rm_encoding), a_engine->codec_params.iananame) ? 0 : 1;
					}

					if (match && connection->c_address && remote_host && !strcmp(connection->c_address, remote_host) && m->m_port == remote_port) {
						same = 1;
					} else {
						same = 0;
						break;
					}
				}

				if (same) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
									  "Our existing sdp is still good [%s %s:%d], let's keep it.\n",
									  a_engine->codec_params.rm_encoding, a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);
					got_audio = 1;
				} else {
					match = 0;
					got_audio = 0;
				}
			}

			for (map = m->m_rtpmaps; map; map = map->rm_next) {
				const char *rm_encoding;
				
				if (!(rm_encoding = map->rm_encoding)) {
					rm_encoding = "";
				}


				if (!strcasecmp(rm_encoding, "telephone-event")) {
					if (!best_te || map->rm_rate == a_engine->codec_params.rm_rate) {
						best_te = (switch_payload_t) map->rm_pt;
					}
				}
				
				if (!switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) && !cng_pt && !strcasecmp(rm_encoding, "CN")) {
					cng_pt = (switch_payload_t) map->rm_pt;
					if (a_engine->rtp_session) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set comfort noise payload to %u\n", cng_pt);
						switch_rtp_set_cng_pt(a_engine->rtp_session, smh->mparams->cng_pt);
					}
				}

			}

			for (map = m->m_rtpmaps; map; map = map->rm_next) {
				int32_t i;
				const char *rm_encoding;
				uint32_t map_bit_rate = 0;
				switch_codec_fmtp_t codec_fmtp = { 0 };

				if (x++ < skip) {
					continue;
				}

				if (!(rm_encoding = map->rm_encoding)) {
					rm_encoding = "";
				}
				
				if (match) {
					continue;
				}

				if (greedy) {
					first = mine;
					last = first + 1;
				} else {
					first = 0;
					last = smh->mparams->num_codecs;
				}

				codec_ms = ptime;

				if (maxptime && (!codec_ms || codec_ms > maxptime)) {
					codec_ms = maxptime;
				}

				if (!codec_ms) {
					codec_ms = switch_default_ptime(rm_encoding, map->rm_pt);
				}

				map_bit_rate = switch_known_bitrate((switch_payload_t)map->rm_pt);
				
				if (!ptime && !strcasecmp(map->rm_encoding, "g723")) {
					codec_ms = 30;
				}
				
				if (zstr(map->rm_fmtp)) {
					if (!strcasecmp(map->rm_encoding, "ilbc")) {
						codec_ms = 30;
						map_bit_rate = 13330;
					} else if (!strcasecmp(map->rm_encoding, "isac")) {
						codec_ms = 30;
						map_bit_rate = 32000;
					}
				} else {
					if ((switch_core_codec_parse_fmtp(map->rm_encoding, map->rm_fmtp, map->rm_rate, &codec_fmtp)) == SWITCH_STATUS_SUCCESS) {
						if (codec_fmtp.bits_per_second) {
							map_bit_rate = codec_fmtp.bits_per_second;
						}
						if (codec_fmtp.microseconds_per_packet) {
							codec_ms = (codec_fmtp.microseconds_per_packet / 1000);
						}
					}
				}

				
				for (i = first; i < last && i < total_codecs; i++) {
					const switch_codec_implementation_t *imp = codec_array[i];
					uint32_t bit_rate = imp->bits_per_second;
					uint32_t codec_rate = imp->samples_per_second;
					if (imp->codec_type != SWITCH_CODEC_TYPE_AUDIO) {
						continue;
					}

					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio Codec Compare [%s:%d:%u:%d:%u]/[%s:%d:%u:%d:%u]\n",
									  rm_encoding, map->rm_pt, (int) map->rm_rate, codec_ms, map_bit_rate,
									  imp->iananame, imp->ianacode, codec_rate, imp->microseconds_per_packet / 1000, bit_rate);
					if ((zstr(map->rm_encoding) || (smh->mparams->ndlb & SM_NDLB_ALLOW_BAD_IANANAME)) && map->rm_pt < 96) {
						match = (map->rm_pt == imp->ianacode) ? 1 : 0;
					} else {
						match = (!strcasecmp(rm_encoding, imp->iananame) && (map->rm_rate == codec_rate)) ? 1 : 0;
					}

					if (match && bit_rate && map_bit_rate && map_bit_rate != bit_rate && strcasecmp(map->rm_encoding, "ilbc") && 
						strcasecmp(map->rm_encoding, "isac")) {
						/* if a bit rate is specified and doesn't match, this is not a codec match, except for ILBC */
						match = 0;
					}

					if (match && map->rm_rate && codec_rate && map->rm_rate != codec_rate && (!strcasecmp(map->rm_encoding, "pcma") || 
																							  !strcasecmp(map->rm_encoding, "pcmu"))) {
						/* if the sampling rate is specified and doesn't match, this is not a codec match for G.711 */
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "sampling rates have to match for G.711\n");
						match = 0;
					}
					
					if (match) {
						if (scrooge) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
											  "Bah HUMBUG! Sticking with %s@%uh@%ui\n",
											  imp->iananame, imp->samples_per_second, imp->microseconds_per_packet / 1000);
						} else if (!near_match) {
							if ((ptime && codec_ms && codec_ms * 1000 != imp->microseconds_per_packet) || map->rm_rate != codec_rate) {
								near_rate = map->rm_rate;
								near_match = imp;
								near_map = mmap = map;
								match = 0;

								if (switch_true(switch_channel_get_variable_dup(channel, "rtp_negotiate_near_match", SWITCH_FALSE, -1))) {
									goto near_match;
								}

								continue;
							}
						}
						mimp = imp;
						mmap = map;
						break;
					}
				}

				
				if (!match && greedy) {
					skip++;
					continue;
				}

				if (match && mimp) {
					break;
				}
			}

		near_match:

			if (!match && near_match) {
				const switch_codec_implementation_t *search[1];
				char *prefs[1];
				char tmp[80];
				int num;
				
				switch_snprintf(tmp, sizeof(tmp), "%s@%uh@%ui", near_match->iananame, near_rate ? near_rate : near_match->samples_per_second,
								codec_ms);
				
				prefs[0] = tmp;
				num = switch_loadable_module_get_codecs_sorted(search, 1, prefs, 1);
				
				if (num) {
					mimp = search[0];
					mmap = map;
				} else {
					mimp = near_match;
					mmap = map;
				}
				
				if (!maxptime || mimp->microseconds_per_packet / 1000 <= maxptime) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Substituting codec %s@%ui@%uh\n",
									  mimp->iananame, mimp->microseconds_per_packet / 1000, mimp->samples_per_second);
					mmap = near_map;
					match = 1;
				} else {
					mimp = NULL;
					mmap = NULL;
					match = 0;
				}
			}
			
			if (mimp && mmap) {
				char tmp[50];
				const char *mirror = switch_channel_get_variable(session->channel, "rtp_mirror_remote_audio_codec_payload");

				a_engine->codec_params.rm_encoding = switch_core_session_strdup(session, (char *) mmap->rm_encoding);
				a_engine->codec_params.iananame = switch_core_session_strdup(session, (char *) mimp->iananame);
				a_engine->codec_params.pt = (switch_payload_t) mmap->rm_pt;
				a_engine->codec_params.rm_rate = mimp->samples_per_second;
				a_engine->codec_params.codec_ms = mimp->microseconds_per_packet / 1000;
				a_engine->codec_params.bitrate = mimp->bits_per_second;
				a_engine->codec_params.channels = mmap->rm_params ? atoi(mmap->rm_params) : 1;

				if (!strcasecmp((char *) mmap->rm_encoding, "opus")) {
					if (a_engine->codec_params.channels == 1) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Invalid SDP for opus.  Don't ask.. but it needs a /2\n");
						a_engine->codec_params.adv_channels = 1;
					} else {
						a_engine->codec_params.adv_channels = 2; /* IKR ???*/
					}
					if (!zstr((char *) mmap->rm_fmtp) && switch_stristr("stereo=1", (char *) mmap->rm_fmtp)) {
						a_engine->codec_params.channels = 2;
					} else {
						a_engine->codec_params.channels = 1;
					}
				} else {
					a_engine->codec_params.adv_channels = a_engine->codec_params.channels;
				}

				a_engine->codec_params.remote_sdp_ip = switch_core_session_strdup(session, (char *) connection->c_address);
				a_engine->codec_params.remote_sdp_port = (switch_port_t) m->m_port;
				a_engine->codec_params.rm_fmtp = switch_core_session_strdup(session, (char *) mmap->rm_fmtp);
				
				a_engine->codec_params.agreed_pt = (switch_payload_t) mmap->rm_pt;
				smh->num_negotiated_codecs = 0;
				smh->negotiated_codecs[smh->num_negotiated_codecs++] = mimp;
				switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.remote_sdp_port);
				switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE, a_engine->codec_params.remote_sdp_ip);
				switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE, tmp);
				a_engine->codec_params.recv_pt = (switch_payload_t)mmap->rm_pt;
					
				if (!switch_true(mirror) && 
					switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND && 
					(!switch_channel_test_flag(session->channel, CF_REINVITE) || switch_media_handle_test_media_flag(smh, SCMF_RENEG_ON_REINVITE))) {
					switch_core_media_get_offered_pt(session, mimp, &a_engine->codec_params.recv_pt);
				}
					
				switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.recv_pt);
				switch_channel_set_variable(session->channel, "rtp_audio_recv_pt", tmp);
					
			}
				
			if (match) {
				if (switch_core_media_set_codec(session, 1, smh->mparams->codec_flags) == SWITCH_STATUS_SUCCESS) {
					got_audio = 1;
					check_ice(smh, SWITCH_MEDIA_TYPE_AUDIO, sdp, m);
				} else {
					match = 0;
				}
			}
				
			if (!best_te && (switch_media_handle_test_media_flag(smh, SCMF_LIBERAL_DTMF) || 
							 switch_channel_test_flag(session->channel, CF_LIBERAL_DTMF))) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
								  "No 2833 in SDP. Liberal DTMF mode adding %d as telephone-event.\n", smh->mparams->te);
				best_te = smh->mparams->te;
			}

			if (best_te) {
				if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
					te = smh->mparams->te = (switch_payload_t) best_te;
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set 2833 dtmf send payload to %u\n", best_te);
					switch_channel_set_variable(session->channel, "dtmf_type", "rfc2833");
					smh->mparams->dtmf_type = DTMF_2833;
					if (a_engine->rtp_session) {
						switch_rtp_set_telephony_event(a_engine->rtp_session, (switch_payload_t) best_te);
						switch_channel_set_variable_printf(session->channel, "rtp_2833_send_payload", "%d", best_te);
					}
				} else {
					te = smh->mparams->recv_te = smh->mparams->te = (switch_payload_t) best_te;
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set 2833 dtmf send/recv payload to %u\n", te);
					switch_channel_set_variable(session->channel, "dtmf_type", "rfc2833");
					smh->mparams->dtmf_type = DTMF_2833;
					if (a_engine->rtp_session) {
						switch_rtp_set_telephony_event(a_engine->rtp_session, te);
						switch_channel_set_variable_printf(session->channel, "rtp_2833_send_payload", "%d", te);
						switch_rtp_set_telephony_recv_event(a_engine->rtp_session, te);
						switch_channel_set_variable_printf(session->channel, "rtp_2833_recv_payload", "%d", te);
					}
				}
			} else {
				/* by default, use SIP INFO if 2833 is not in the SDP */
				if (!switch_false(switch_channel_get_variable(channel, "rtp_info_when_no_2833"))) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "No 2833 in SDP.  Disable 2833 dtmf and switch to INFO\n");
					switch_channel_set_variable(session->channel, "dtmf_type", "info");
					smh->mparams->dtmf_type = DTMF_INFO;
					te = smh->mparams->recv_te = smh->mparams->te = 0;
				} else {
					switch_channel_set_variable(session->channel, "dtmf_type", "none");
					smh->mparams->dtmf_type = DTMF_NONE;
					te = smh->mparams->recv_te = smh->mparams->te = 0;
				}
			}

			
			if (!match && greedy && mine < total_codecs) {
				mine++;
				skip = 0;
				goto greed;
			}

		} else if (m->m_type == sdp_media_video && m->m_port) {
			sdp_rtpmap_t *map;
			const char *rm_encoding;
			const switch_codec_implementation_t *mimp = NULL;
			int vmatch = 0, i;
			switch_channel_set_variable(session->channel, "video_possible", "true");

			connection = sdp->sdp_connection;
			if (m->m_connections) {
				connection = m->m_connections;
			}

			if (!connection) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Cannot find a c= line in the sdp at media or session level!\n");
				match = 0;
				break;
			}

			for (map = m->m_rtpmaps; map; map = map->rm_next) {

				if (switch_rtp_has_dtls() && dtls_ok(session)) {
					for (attr = m->m_attributes; attr; attr = attr->a_next) {
						if (!strcasecmp(attr->a_name, "fingerprint") && !zstr(attr->a_value)) {
							got_video_crypto = 1;
						}
					}
				}

				for (attr = m->m_attributes; attr; attr = attr->a_next) {
					if (!strcasecmp(attr->a_name, "framerate") && attr->a_value) {
						//framerate = atoi(attr->a_value);
					}
					if (!strcasecmp(attr->a_name, "rtcp") && attr->a_value && !strcmp(attr->a_value, "1")) {
						switch_channel_set_variable(session->channel, "rtp_remote_video_rtcp_port", attr->a_value);
						v_engine->remote_rtcp_port = (switch_port_t)atoi(attr->a_value);
					} else if (!got_video_crypto && !strcasecmp(attr->a_name, "crypto") && !zstr(attr->a_value)) {
						int crypto_tag;
						
						if (!(smh->mparams->ndlb & SM_NDLB_ALLOW_CRYPTO_IN_AVP) && 
							!switch_true(switch_channel_get_variable(session->channel, "rtp_allow_crypto_in_avp"))) {
							if (m->m_proto != sdp_proto_srtp && !got_webrtc) {
								switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "a=crypto in RTP/AVP, refer to rfc3711\n");
								match = 0;
								goto done;
							}
						}
						
						crypto = attr->a_value;
						crypto_tag = atoi(crypto);
						
						got_video_crypto = switch_core_session_check_incoming_crypto(session, 
																					 "rtp_has_video_crypto", 
																					 SWITCH_MEDIA_TYPE_VIDEO, crypto, crypto_tag, sdp_type);
					
					}
				}
				
				if (got_video_crypto && !got_video_avp) {
					switch_channel_set_variable(session->channel, "rtp_crypto_mandatory", "true");
					switch_channel_set_variable(session->channel, "rtp_secure_media", "true");
				}

				if (!(rm_encoding = map->rm_encoding)) {
					rm_encoding = "";
				}

				for (i = 0; i < total_codecs; i++) {
					const switch_codec_implementation_t *imp = codec_array[i];

					if (imp->codec_type != SWITCH_CODEC_TYPE_VIDEO) { 
						continue;
					}

					if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND &&
						switch_channel_test_flag(session->channel, CF_NOVIDEO)) {
						continue;
					}

					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Video Codec Compare [%s:%d]/[%s:%d]\n",
									  rm_encoding, map->rm_pt, imp->iananame, imp->ianacode);
					if ((zstr(map->rm_encoding) || (smh->mparams->ndlb & SM_NDLB_ALLOW_BAD_IANANAME)) && map->rm_pt < 96) {
						vmatch = (map->rm_pt == imp->ianacode) ? 1 : 0;
					} else {
						vmatch = strcasecmp(rm_encoding, imp->iananame) ? 0 : 1;
					}


					if (vmatch && (map->rm_rate == imp->samples_per_second)) {
						mimp = imp;
						break;
					} else {
						vmatch = 0;
					}
				}

				if (mimp) {
					if ((v_engine->codec_params.rm_encoding = switch_core_session_strdup(session, (char *) rm_encoding))) {
						char tmp[50];
						const char *mirror = switch_channel_get_variable(session->channel, "rtp_mirror_remote_video_codec_payload");

						v_engine->codec_params.pt = (switch_payload_t) map->rm_pt;
						v_engine->codec_params.rm_rate = map->rm_rate;
						v_engine->codec_params.codec_ms = mimp->microseconds_per_packet / 1000;


						v_engine->codec_params.remote_sdp_ip = switch_core_session_strdup(session, (char *) connection->c_address);
						v_engine->codec_params.remote_sdp_port = (switch_port_t) m->m_port;
						
						v_engine->codec_params.rm_fmtp = switch_core_session_strdup(session, (char *) map->rm_fmtp);

						v_engine->codec_params.agreed_pt = (switch_payload_t) map->rm_pt;
						switch_snprintf(tmp, sizeof(tmp), "%d", v_engine->codec_params.remote_sdp_port);
						switch_channel_set_variable(session->channel, SWITCH_REMOTE_VIDEO_IP_VARIABLE, v_engine->codec_params.remote_sdp_ip);
						switch_channel_set_variable(session->channel, SWITCH_REMOTE_VIDEO_PORT_VARIABLE, tmp);
						switch_channel_set_variable(session->channel, "rtp_video_fmtp", v_engine->codec_params.rm_fmtp);
						switch_snprintf(tmp, sizeof(tmp), "%d", v_engine->codec_params.agreed_pt);
						switch_channel_set_variable(session->channel, "rtp_video_pt", tmp);
						switch_core_media_check_video_codecs(session);

						v_engine->codec_params.recv_pt = (switch_payload_t)map->rm_pt;
						
						if (!switch_true(mirror) && switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
							switch_core_media_get_offered_pt(session, mimp, &v_engine->codec_params.recv_pt);
						}

						switch_snprintf(tmp, sizeof(tmp), "%d", v_engine->codec_params.recv_pt);
						switch_channel_set_variable(session->channel, "rtp_video_recv_pt", tmp);
						if (!match && vmatch) match = 1;

						check_ice(smh, SWITCH_MEDIA_TYPE_VIDEO, sdp, m);
						//check_ice(smh, SWITCH_MEDIA_TYPE_VIDEO, sdp, NULL);
						break;
					} else {
						vmatch = 0;
					}
				}
			}
			
		}
	}

 done:

	if (parser) {
		sdp_parser_free(parser);
	}

	smh->mparams->cng_pt = cng_pt;

	return match;
}

//?

SWITCH_DECLARE(int) switch_core_media_toggle_hold(switch_core_session_t *session, int sendonly)
{
	int changed = 0;
	switch_rtp_engine_t *a_engine;//, *v_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return 0;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	//v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	if (switch_channel_test_flag(session->channel, CF_SLA_BARGE) || switch_channel_test_flag(session->channel, CF_SLA_BARGING)) {
		switch_channel_mark_hold(session->channel, sendonly);
		return 0;
	}

	if (sendonly && switch_channel_test_flag(session->channel, CF_ANSWERED)) {
		if (!switch_channel_test_flag(session->channel, CF_PROTO_HOLD)) {
			const char *stream;
			const char *msg = "hold";
			const char *info = switch_channel_get_variable(session->channel, "presence_call_info");

			if (info) {
				if (switch_stristr("private", info)) {
					msg = "hold-private";
				}
			}
			

			switch_channel_set_flag(session->channel, CF_PROTO_HOLD);
			switch_channel_mark_hold(session->channel, SWITCH_TRUE);
			switch_channel_presence(session->channel, "unknown", msg, NULL);
			changed = 1;

			if (a_engine->max_missed_hold_packets) {
				switch_rtp_set_max_missed_packets(a_engine->rtp_session, a_engine->max_missed_hold_packets);
			}

			if (!(stream = switch_channel_get_hold_music(session->channel))) {
				stream = "local_stream://moh";
			}

			if (stream && strcasecmp(stream, "silence")) {
				if (!strcasecmp(stream, "indicate_hold")) {
					switch_channel_set_flag(session->channel, CF_SUSPEND);
					switch_channel_set_flag(session->channel, CF_HOLD);
					switch_ivr_hold_uuid(switch_channel_get_partner_uuid(session->channel), NULL, 0);
				} else {
					switch_ivr_broadcast(switch_channel_get_partner_uuid(session->channel), stream,
										 SMF_ECHO_ALEG | SMF_LOOP | SMF_PRIORITY);
					switch_yield(250000);
				}
			}
		}
	} else {
		if (switch_channel_test_flag(session->channel, CF_HOLD_LOCK)) {
			switch_channel_set_flag(session->channel, CF_PROTO_HOLD);
			switch_channel_mark_hold(session->channel, SWITCH_TRUE);
			changed = 1;
		}

		switch_channel_clear_flag(session->channel, CF_HOLD_LOCK);

		if (switch_channel_test_flag(session->channel, CF_PROTO_HOLD)) {
			const char *uuid;
			switch_core_session_t *b_session;

			switch_yield(250000);

			if (a_engine->max_missed_packets) {
				switch_rtp_reset_media_timer(a_engine->rtp_session);
				switch_rtp_set_max_missed_packets(a_engine->rtp_session, a_engine->max_missed_packets);
			}

			if ((uuid = switch_channel_get_partner_uuid(session->channel)) && (b_session = switch_core_session_locate(uuid))) {
				switch_channel_t *b_channel = switch_core_session_get_channel(b_session);

				if (switch_channel_test_flag(session->channel, CF_HOLD)) {
					switch_ivr_unhold(b_session);
					switch_channel_clear_flag(session->channel, CF_SUSPEND);
					switch_channel_clear_flag(session->channel, CF_HOLD);
				} else {
					switch_channel_stop_broadcast(b_channel);
					switch_channel_wait_for_flag(b_channel, CF_BROADCAST, SWITCH_FALSE, 5000, NULL);
				}
				switch_core_session_rwunlock(b_session);
			}

			switch_channel_clear_flag(session->channel, CF_PROTO_HOLD);
			switch_channel_mark_hold(session->channel, SWITCH_FALSE);
			switch_channel_presence(session->channel, "unknown", "unhold", NULL);
			changed = 1;
		}
	}

	return changed;
}


//?
#define RA_PTR_LEN 512
SWITCH_DECLARE(switch_status_t) switch_core_media_proxy_remote_addr(switch_core_session_t *session, const char *sdp_str)
{
	const char *err;
	char rip[RA_PTR_LEN] = "";
	char rp[RA_PTR_LEN] = "";
	char rvp[RA_PTR_LEN] = "";
	char *p, *ip_ptr = NULL, *port_ptr = NULL, *vid_port_ptr = NULL, *pe;
	int x;
	const char *val;
	switch_status_t status = SWITCH_STATUS_FALSE;
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];
	
	if (zstr(sdp_str)) {
		sdp_str = smh->mparams->remote_sdp_str;
	}

	if (zstr(sdp_str)) {
		goto end;
	}

	if ((p = (char *) switch_stristr("c=IN IP4 ", sdp_str)) || (p = (char *) switch_stristr("c=IN IP6 ", sdp_str))) {
		ip_ptr = p + 9;
	}

	if ((p = (char *) switch_stristr("m=audio ", sdp_str))) {
		port_ptr = p + 8;
	}

	if ((p = (char *) switch_stristr("m=image ", sdp_str))) {
		char *tmp = p + 8;
		
		if (tmp && atoi(tmp)) {
			port_ptr = tmp;
		}
	}

	if ((p = (char *) switch_stristr("m=video ", sdp_str))) {
		vid_port_ptr = p + 8;
	}

	if (!(ip_ptr && port_ptr)) {
		goto end;
	}

	p = ip_ptr;
	pe = p + strlen(p);
	x = 0;
	while (x < sizeof(rip) - 1 && p && *p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == ':' || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
		rip[x++] = *p;
		p++;
		if (p >= pe) {
			goto end;
		}
	}

	p = port_ptr;
	x = 0;
	while (x < sizeof(rp) - 1 && p && *p && (*p >= '0' && *p <= '9')) {
		rp[x++] = *p;
		p++;
		if (p >= pe) {
			goto end;
		}
	}

	p = vid_port_ptr;
	x = 0;
	while (x < sizeof(rvp) - 1 && p && *p && (*p >= '0' && *p <= '9')) {
		rvp[x++] = *p;
		p++;
		if (p >= pe) {
			goto end;
		}
	}

	if (!(*rip && *rp)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "invalid SDP\n");
		goto end;
	}

	a_engine->codec_params.remote_sdp_ip = switch_core_session_strdup(session, rip);
	a_engine->codec_params.remote_sdp_port = (switch_port_t) atoi(rp);

	if (*rvp) {
		v_engine->codec_params.remote_sdp_ip = switch_core_session_strdup(session, rip);
		v_engine->codec_params.remote_sdp_port = (switch_port_t) atoi(rvp);
	}

	if (v_engine->codec_params.remote_sdp_ip && v_engine->codec_params.remote_sdp_port) {
		if (!strcmp(v_engine->codec_params.remote_sdp_ip, rip) && atoi(rvp) == v_engine->codec_params.remote_sdp_port) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Remote video address:port [%s:%d] has not changed.\n",
							  v_engine->codec_params.remote_sdp_ip, v_engine->codec_params.remote_sdp_port);
		} else {
			switch_channel_set_flag(session->channel, CF_VIDEO_POSSIBLE);
			switch_channel_set_flag(session->channel, CF_VIDEO);
			if (switch_rtp_ready(v_engine->rtp_session)) {
				const char *rport = NULL;
				switch_port_t remote_rtcp_port = v_engine->remote_rtcp_port;

				if (!remote_rtcp_port) {
					if ((rport = switch_channel_get_variable(session->channel, "rtp_remote_video_rtcp_port"))) {
						remote_rtcp_port = (switch_port_t)atoi(rport);
					}
				}


				if (switch_rtp_set_remote_address(v_engine->rtp_session, v_engine->codec_params.remote_sdp_ip,
												  v_engine->codec_params.remote_sdp_port, remote_rtcp_port, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "VIDEO RTP REPORTS ERROR: [%s]\n", err);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "VIDEO RTP CHANGING DEST TO: [%s:%d]\n",
									  v_engine->codec_params.remote_sdp_ip, v_engine->codec_params.remote_sdp_port);
					if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_PROXY_MODE) &&
						!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val)) && 
						!switch_channel_test_flag(session->channel, CF_WEBRTC)) {
						/* Reactivate the NAT buster flag. */
						switch_rtp_set_flag(v_engine->rtp_session, SWITCH_RTP_FLAG_AUTOADJ);
					}
					if (switch_media_handle_test_media_flag(smh, SCMF_AUTOFIX_TIMING)) {
						v_engine->check_frames = 0;
					}
				}
			}
		}
	}

	if (switch_rtp_ready(a_engine->rtp_session)) {
		char *remote_host = switch_rtp_get_remote_host(a_engine->rtp_session);
		switch_port_t remote_port = switch_rtp_get_remote_port(a_engine->rtp_session);
		const char *rport = NULL;
		switch_port_t remote_rtcp_port = 0;

		if (remote_host && remote_port && !strcmp(remote_host, a_engine->codec_params.remote_sdp_ip) && remote_port == a_engine->codec_params.remote_sdp_port) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Remote address:port [%s:%d] has not changed.\n",
							  a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);
			switch_goto_status(SWITCH_STATUS_BREAK, end);
		}

		if ((rport = switch_channel_get_variable(session->channel, "rtp_remote_audio_rtcp_port"))) {
			remote_rtcp_port = (switch_port_t)atoi(rport);
		}


		if (switch_rtp_set_remote_address(a_engine->rtp_session, a_engine->codec_params.remote_sdp_ip,
										  a_engine->codec_params.remote_sdp_port, remote_rtcp_port, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AUDIO RTP REPORTS ERROR: [%s]\n", err);
			status = SWITCH_STATUS_GENERR;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AUDIO RTP CHANGING DEST TO: [%s:%d]\n",
							  a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);
			if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) &&
				!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val)) && 
				!switch_channel_test_flag(session->channel, CF_WEBRTC)) {
				/* Reactivate the NAT buster flag. */
				switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_AUTOADJ);
			}
			if (switch_media_handle_test_media_flag(smh, SCMF_AUTOFIX_TIMING)) {
				a_engine->check_frames = 0;
			}
			status = SWITCH_STATUS_SUCCESS;
		}
	}

 end:

	return status;
}

//?
SWITCH_DECLARE(int) switch_core_media_check_nat(switch_media_handle_t *smh, const char *network_ip)
{
	switch_assert(network_ip);

	return (smh->mparams->extsipip && 
			!switch_check_network_list_ip(network_ip, "loopback.auto") && 
			!switch_check_network_list_ip(network_ip, smh->mparams->local_network));
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_ext_address_lookup(switch_core_session_t *session, char **ip, switch_port_t *port, const char *sourceip)
															  
{
	char *error = "";
	switch_status_t status = SWITCH_STATUS_FALSE;
	int x;
	switch_port_t myport = *port;
	switch_port_t stun_port = SWITCH_STUN_DEFAULT_PORT;
	char *stun_ip = NULL;
	switch_media_handle_t *smh;
	switch_memory_pool_t *pool = switch_core_session_get_pool(session);

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (!sourceip) {
		return status;
	}

	if (!strncasecmp(sourceip, "host:", 5)) {
		status = (*ip = switch_stun_host_lookup(sourceip + 5, pool)) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
	} else if (!strncasecmp(sourceip, "stun:", 5)) {
		char *p;

		stun_ip = strdup(sourceip + 5);

		if ((p = strchr(stun_ip, ':'))) {
			int iport;
			*p++ = '\0';
			iport = atoi(p);
			if (iport > 0 && iport < 0xFFFF) {
				stun_port = (switch_port_t) iport;
			}
		}

		if (zstr(stun_ip)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "STUN Failed! NO STUN SERVER\n");
			goto out;
		}

		for (x = 0; x < 5; x++) {
			if ((status = switch_stun_lookup(ip, port, stun_ip, stun_port, &error, pool)) != SWITCH_STATUS_SUCCESS) {
				switch_yield(100000);
			} else {
				break;
			}
		}
		if (status != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "STUN Failed! %s:%d [%s]\n", stun_ip, stun_port, error);
			goto out;
		}
		if (!*ip) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "STUN Failed! No IP returned\n");
			goto out;
		}
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "STUN Success [%s]:[%d]\n", *ip, *port);
		status = SWITCH_STATUS_SUCCESS;

		if (myport == *port && !strcmp(*ip, smh->mparams->rtpip)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "STUN Not Required ip and port match. [%s]:[%d]\n", *ip, *port);
		} else {
			smh->mparams->stun_ip = switch_core_session_strdup(session, stun_ip);
			smh->mparams->stun_port = stun_port;
			smh->mparams->stun_flags |= STUN_FLAG_SET;
		}
	} else {
		*ip = (char *) sourceip;
		status = SWITCH_STATUS_SUCCESS;
	}

 out:

	switch_safe_free(stun_ip);

	return status;
}

//?
SWITCH_DECLARE(void) switch_core_media_reset_autofix_timing(switch_core_session_t *session, switch_media_type_t type)
{
	switch_rtp_engine_t *engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	engine = &smh->engines[type];

	engine->check_frames = 0;
	engine->last_ts = 0;
}



//?
SWITCH_DECLARE(switch_status_t) switch_core_media_choose_port(switch_core_session_t *session, switch_media_type_t type, int force)
{
	char *lookup_rtpip;	/* Pointer to externally looked up address */
	switch_port_t sdp_port;		/* The external port to be sent in the SDP */
	const char *use_ip = NULL;	/* The external IP to be sent in the SDP */
	switch_rtp_engine_t *engine;
	switch_media_handle_t *smh;
	const char *tstr = switch_media_type2str(type);
	char vname[128] = "";

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	engine = &smh->engines[type];

	lookup_rtpip = smh->mparams->rtpip;

	/* Don't do anything if we're in proxy mode or if a (remote) port already has been found */
	if (!force) {
		if (switch_channel_test_flag(session->channel, CF_PROXY_MODE) ||
			switch_channel_test_flag(session->channel, CF_PROXY_MEDIA) || engine->codec_params.adv_sdp_port) {
			return SWITCH_STATUS_SUCCESS;
		}
	}

	/* Release the local sdp port */
	if (engine->codec_params.local_sdp_port) {
		switch_rtp_release_port(smh->mparams->rtpip, engine->codec_params.local_sdp_port);
	}

	/* Request a local port from the core's allocator */
	if (!(engine->codec_params.local_sdp_port = switch_rtp_request_port(smh->mparams->rtpip))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_CRIT, "No %s RTP ports available!\n", tstr);
		return SWITCH_STATUS_FALSE;
	}

	engine->codec_params.local_sdp_ip = smh->mparams->rtpip;
	

	sdp_port = engine->codec_params.local_sdp_port;

	/* Check if NAT is detected  */
	if (!zstr(smh->mparams->remote_ip) && switch_core_media_check_nat(smh, smh->mparams->remote_ip)) {
		/* Yes, map the port through switch_nat */
		switch_nat_add_mapping(engine->codec_params.local_sdp_port, SWITCH_NAT_UDP, &sdp_port, SWITCH_FALSE);

		switch_snprintf(vname, sizeof(vname), "rtp_adv_%s_ip", tstr);
		
		/* Find an IP address to use */
		if (!(use_ip = switch_channel_get_variable(session->channel, vname))
			&& !zstr(smh->mparams->extrtpip)) {
			use_ip = smh->mparams->extrtpip;
		}

		if (use_ip) {
			if (switch_core_media_ext_address_lookup(session, &lookup_rtpip, &sdp_port, use_ip) != SWITCH_STATUS_SUCCESS) {
				/* Address lookup was required and fail (external ip was "host:..." or "stun:...") */
				return SWITCH_STATUS_FALSE;
			} else {
				/* Address properly resolved, use it as external ip */
				use_ip = lookup_rtpip;
			}
		} else {
			/* No external ip found, use the profile's rtp ip */
			use_ip = smh->mparams->rtpip;
		}
	} else {
		/* No NAT traversal required, use the profile's rtp ip */
		use_ip = smh->mparams->rtpip;
	}

	engine->codec_params.adv_sdp_port = sdp_port;
	engine->codec_params.adv_sdp_ip = smh->mparams->adv_sdp_audio_ip = smh->mparams->extrtpip = switch_core_session_strdup(session, use_ip);


	if (type == SWITCH_MEDIA_TYPE_AUDIO) {
		switch_channel_set_variable(session->channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE, engine->codec_params.local_sdp_ip);
		switch_channel_set_variable_printf(session->channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE, "%d", sdp_port);
		switch_channel_set_variable(session->channel, SWITCH_ADVERTISED_MEDIA_IP_VARIABLE, engine->codec_params.adv_sdp_ip);
	} else {
		switch_channel_set_variable(session->channel, SWITCH_LOCAL_VIDEO_IP_VARIABLE, engine->codec_params.adv_sdp_ip);
		switch_channel_set_variable_printf(session->channel, SWITCH_LOCAL_VIDEO_PORT_VARIABLE, "%d", sdp_port);
	}

	return SWITCH_STATUS_SUCCESS;
}

//?
SWITCH_DECLARE(void) switch_core_media_deactivate_rtp(switch_core_session_t *session)
{
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	if (v_engine->media_thread) {
		switch_status_t st;
		switch_channel_clear_flag(session->channel, CF_VIDEO_PASSIVE);
		
		v_engine->mh.up = 0;
		switch_thread_join(&st, v_engine->media_thread);
		v_engine->media_thread = NULL;
	}

	if (v_engine->rtp_session) {
		switch_rtp_destroy(&v_engine->rtp_session);
	} else if (v_engine->codec_params.local_sdp_port) {
		switch_rtp_release_port(smh->mparams->rtpip, v_engine->codec_params.local_sdp_port);
	}


	if (v_engine->codec_params.local_sdp_port > 0 && !zstr(smh->mparams->remote_ip) && 
		switch_core_media_check_nat(smh, smh->mparams->remote_ip)) {
		switch_nat_del_mapping((switch_port_t) v_engine->codec_params.local_sdp_port, SWITCH_NAT_UDP);
		switch_nat_del_mapping((switch_port_t) v_engine->codec_params.local_sdp_port + 1, SWITCH_NAT_UDP);
	}


	if (a_engine->rtp_session) {
		switch_rtp_destroy(&a_engine->rtp_session);
	} else if (a_engine->codec_params.local_sdp_port) {
		switch_rtp_release_port(smh->mparams->rtpip, a_engine->codec_params.local_sdp_port);
	}

	if (a_engine->codec_params.local_sdp_port > 0 && !zstr(smh->mparams->remote_ip) && 
		switch_core_media_check_nat(smh, smh->mparams->remote_ip)) {
		switch_nat_del_mapping((switch_port_t) a_engine->codec_params.local_sdp_port, SWITCH_NAT_UDP);
		switch_nat_del_mapping((switch_port_t) a_engine->codec_params.local_sdp_port + 1, SWITCH_NAT_UDP);
	}

}


//?
static void gen_ice(switch_core_session_t *session, switch_media_type_t type, const char *ip, switch_port_t port)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *engine;
	char tmp[33] = "";

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	engine = &smh->engines[type];

#ifdef RTCP_MUX
	if (!engine->rtcp_mux) {//  && type == SWITCH_MEDIA_TYPE_AUDIO) {
		engine->rtcp_mux = SWITCH_TRUE;
	}
#endif

	if (!smh->msid) {
		switch_stun_random_string(tmp, 32, NULL);
		tmp[32] = '\0';
		smh->msid = switch_core_session_strdup(session, tmp);
	}

	if (!smh->cname) {
		switch_stun_random_string(tmp, 16, NULL);
		tmp[16] = '\0';
		smh->cname = switch_core_session_strdup(session, tmp);
	}
	
	if (!engine->ice_out.ufrag) {
		switch_stun_random_string(tmp, 16, NULL);
		tmp[16] = '\0';
		engine->ice_out.ufrag = switch_core_session_strdup(session, tmp);
	}

	if (!engine->ice_out.pwd) {
		switch_stun_random_string(tmp, 16, NULL);
		engine->ice_out.pwd = switch_core_session_strdup(session, tmp);	
	}

	if (!engine->ice_out.cands[0][0].foundation) {
		switch_stun_random_string(tmp, 10, "0123456789");
		tmp[10] = '\0';
		engine->ice_out.cands[0][0].foundation = switch_core_session_strdup(session, tmp);
	}

	engine->ice_out.cands[0][0].transport = "udp";

	if (!engine->ice_out.cands[0][0].component_id) {
		engine->ice_out.cands[0][0].component_id = 1;
		engine->ice_out.cands[0][0].priority = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - engine->ice_out.cands[0][0].component_id);
	}

	if (!zstr(ip)) {
		engine->ice_out.cands[0][0].con_addr = switch_core_session_strdup(session, ip);
	}

	if (port) {
		engine->ice_out.cands[0][0].con_port = port;
	}

	engine->ice_out.cands[0][0].generation = "0";
	//add rport stuff later
	
	engine->ice_out.cands[0][0].ready = 1;


}

SWITCH_DECLARE(void) switch_core_session_wake_video_thread(switch_core_session_t *session)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *v_engine; 

	if (!(smh = session->media_handle)) {
		return;
	}

	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	if (!v_engine->rtp_session) {
		return;
	}

	if (switch_mutex_trylock(v_engine->mh.cond_mutex) == SWITCH_STATUS_SUCCESS) {
		switch_thread_cond_broadcast(v_engine->mh.cond);
		switch_mutex_unlock(v_engine->mh.cond_mutex);
	}
}

static void *SWITCH_THREAD_FUNC video_helper_thread(switch_thread_t *thread, void *obj)
{
	struct media_helper *mh = obj;
	switch_core_session_t *session = mh->session;
	switch_channel_t *channel = switch_core_session_get_channel(session);
	switch_status_t status;
	switch_frame_t *read_frame;
	switch_media_handle_t *smh;

	if (!(smh = session->media_handle)) {
		return NULL;
	}

	switch_core_session_read_lock(session);

	mh->up = 1;
	switch_mutex_lock(mh->cond_mutex);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s Video thread started\n", switch_channel_get_name(session->channel));
	switch_core_session_refresh_video(session);

	while (switch_channel_up_nosig(channel)) {

		if (switch_channel_test_flag(channel, CF_VIDEO_PASSIVE)) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s Video thread paused. Echo is %s\n", 
							  switch_channel_get_name(session->channel), switch_channel_test_flag(channel, CF_VIDEO_ECHO) ? "on" : "off");
			switch_thread_cond_wait(mh->cond, mh->cond_mutex);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s Video thread resumed  Echo is %s\n", 
							  switch_channel_get_name(session->channel), switch_channel_test_flag(channel, CF_VIDEO_ECHO) ? "on" : "off");
			switch_core_session_refresh_video(session);
		}

		if (switch_channel_test_flag(channel, CF_VIDEO_PASSIVE)) {
			continue;
		}

		if (!switch_channel_media_up(session->channel)) {
			switch_yield(10000);
			continue;
		}

		
		status = switch_core_session_read_video_frame(session, &read_frame, SWITCH_IO_FLAG_NONE, 0);
		
		if (!SWITCH_READ_ACCEPTABLE(status)) {
			switch_cond_next();
			continue;
		}
		

		if (switch_channel_test_flag(channel, CF_VIDEO_REFRESH_REQ)) {
			switch_core_session_refresh_video(session);
			switch_channel_clear_flag(channel, CF_VIDEO_REFRESH_REQ);
		}

		if (switch_test_flag(read_frame, SFF_CNG)) {
			continue;
		}

		if (switch_channel_test_flag(channel, CF_VIDEO_ECHO)) {
			switch_core_session_write_video_frame(session, read_frame, SWITCH_IO_FLAG_NONE, 0);
		}

	}


	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "%s Video thread ended\n", switch_channel_get_name(session->channel));

	switch_mutex_unlock(mh->cond_mutex);
	switch_core_session_rwunlock(session);

	mh->up = 0;
	return NULL;
}




//?
SWITCH_DECLARE(switch_status_t) switch_core_media_activate_rtp(switch_core_session_t *session)

{
	const char *err = NULL;
	const char *val = NULL;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	char tmp[50];
	char *timer_name = NULL;
	const char *var;
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];


	if (switch_channel_down(session->channel)) {
		return SWITCH_STATUS_FALSE;
	}


	if (switch_rtp_ready(a_engine->rtp_session)) {
		switch_rtp_reset_media_timer(a_engine->rtp_session);
	}

	if ((var = switch_channel_get_variable(session->channel, "rtp_secure_media")) && switch_true(var)) {
		switch_channel_set_flag(session->channel, CF_SECURE);
	}

	if (switch_channel_test_flag(session->channel, CF_PROXY_MODE)) {
		status = SWITCH_STATUS_SUCCESS;
		goto end;
	}

	if (!switch_channel_test_flag(session->channel, CF_REINVITE)) {
		if (switch_rtp_ready(a_engine->rtp_session)) {
			if (switch_channel_test_flag(session->channel, CF_VIDEO_POSSIBLE) && !switch_rtp_ready(v_engine->rtp_session)) {
				goto video;
			}

			status = SWITCH_STATUS_SUCCESS;
			goto end;
		} 
	}

	if ((status = switch_core_media_set_codec(session, 0, smh->mparams->codec_flags)) != SWITCH_STATUS_SUCCESS) {
		goto end;
	}

	memset(flags, 0, sizeof(flags));
	flags[SWITCH_RTP_FLAG_DATAWAIT]++;

	if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_WEBRTC) &&
		!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val))) {
		flags[SWITCH_RTP_FLAG_AUTOADJ]++;
	}

	if (switch_media_handle_test_media_flag(smh, SCMF_PASS_RFC2833)
		|| ((val = switch_channel_get_variable(session->channel, "pass_rfc2833")) && switch_true(val))) {
		switch_channel_set_flag(session->channel, CF_PASS_RFC2833);
	}


	if (switch_media_handle_test_media_flag(smh, SCMF_AUTOFLUSH)
		|| ((val = switch_channel_get_variable(session->channel, "rtp_autoflush")) && switch_true(val))) {
		flags[SWITCH_RTP_FLAG_AUTOFLUSH]++;
	}

	if (!(switch_media_handle_test_media_flag(smh, SCMF_REWRITE_TIMESTAMPS) ||
		  ((val = switch_channel_get_variable(session->channel, "rtp_rewrite_timestamps")) && switch_true(val)))) {
		flags[SWITCH_RTP_FLAG_RAW_WRITE]++;
	}

	if (switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG)) {
		smh->mparams->cng_pt = 0;
	} else if (smh->mparams->cng_pt) {
		flags[SWITCH_RTP_FLAG_AUTO_CNG]++;
	}

#if __BYTE_ORDER == __LITTLE_ENDIAN
	if (!strcasecmp(a_engine->read_impl.iananame, "L16")) {
		flags[SWITCH_RTP_FLAG_BYTESWAP]++;
	}
#endif
	
	if ((flags[SWITCH_RTP_FLAG_BYTESWAP]) && (val = switch_channel_get_variable(session->channel, "rtp_disable_byteswap")) && switch_true(val)) {
		flags[SWITCH_RTP_FLAG_BYTESWAP] = 0;
	}

	if (a_engine->rtp_session && switch_channel_test_flag(session->channel, CF_REINVITE)) {
		//const char *ip = switch_channel_get_variable(session->channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE);
		//const char *port = switch_channel_get_variable(session->channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE);
		char *remote_host = switch_rtp_get_remote_host(a_engine->rtp_session);
		switch_port_t remote_port = switch_rtp_get_remote_port(a_engine->rtp_session);

		if (remote_host && remote_port && !strcmp(remote_host, a_engine->codec_params.remote_sdp_ip) && remote_port == a_engine->codec_params.remote_sdp_port) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio params are unchanged for %s.\n",
							  switch_channel_get_name(session->channel));
			if (switch_rtp_ready(a_engine->rtp_session)) {
				if (a_engine->codec_params.recv_pt != a_engine->codec_params.agreed_pt) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
								  "%s Set audio receive payload in Re-INVITE for non-matching dynamic PT to %u\n", 
									  switch_channel_get_name(session->channel), a_engine->codec_params.recv_pt);
				
					switch_rtp_set_recv_pt(a_engine->rtp_session, a_engine->codec_params.recv_pt);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
								  "%s Setting audio receive payload in Re-INVITE to %u\n", 
									  switch_channel_get_name(session->channel), a_engine->codec_params.recv_pt);
					switch_rtp_set_recv_pt(a_engine->rtp_session, a_engine->codec_params.agreed_pt);
				}

			}
			goto video;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Audio params changed for %s from %s:%d to %s:%d\n",
							  switch_channel_get_name(session->channel),
							  remote_host, remote_port, a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);

			switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.remote_sdp_port);
			switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE, a_engine->codec_params.remote_sdp_ip);
			switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE, tmp);
		}
	}

	if (!switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AUDIO RTP [%s] %s port %d -> %s port %d codec: %u ms: %d\n",
						  switch_channel_get_name(session->channel),
						  a_engine->codec_params.local_sdp_ip,
						  a_engine->codec_params.local_sdp_port,
						  a_engine->codec_params.remote_sdp_ip,
						  a_engine->codec_params.remote_sdp_port, a_engine->codec_params.agreed_pt, a_engine->read_impl.microseconds_per_packet / 1000);

		if (switch_rtp_ready(a_engine->rtp_session)) {
			switch_rtp_set_default_payload(a_engine->rtp_session, a_engine->codec_params.agreed_pt);

			if (a_engine->codec_params.recv_pt != a_engine->codec_params.agreed_pt) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
								  "%s Set audio receive payload to %u\n", switch_channel_get_name(session->channel), a_engine->codec_params.recv_pt);
				
				switch_rtp_set_recv_pt(a_engine->rtp_session, a_engine->codec_params.recv_pt);
			} else {
				switch_rtp_set_recv_pt(a_engine->rtp_session, a_engine->codec_params.agreed_pt);
			}

		}
	}

	switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.local_sdp_port);
	switch_channel_set_variable(session->channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE, a_engine->codec_params.local_sdp_ip);
	switch_channel_set_variable(session->channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE, tmp);
	switch_channel_set_variable(session->channel, SWITCH_ADVERTISED_MEDIA_IP_VARIABLE, a_engine->codec_params.adv_sdp_ip);

	if (a_engine->rtp_session && switch_channel_test_flag(session->channel, CF_REINVITE)) {
		const char *rport = NULL;
		switch_port_t remote_rtcp_port = a_engine->remote_rtcp_port;
				
		if (!remote_rtcp_port) {
			if ((rport = switch_channel_get_variable(session->channel, "rtp_remote_audio_rtcp_port"))) {
				remote_rtcp_port = (switch_port_t)atoi(rport);
			}
		}

		if (switch_rtp_set_remote_address(a_engine->rtp_session, a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port,
										  remote_rtcp_port, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AUDIO RTP REPORTS ERROR: [%s]\n", err);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AUDIO RTP CHANGING DEST TO: [%s:%d]\n",
							  a_engine->codec_params.remote_sdp_ip, a_engine->codec_params.remote_sdp_port);
			if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) &&
				!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val)) &&
				!switch_channel_test_flag(session->channel, CF_WEBRTC)) {
				/* Reactivate the NAT buster flag. */
				switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_AUTOADJ);
			}
		}
		goto video;
	}

	if (switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
		switch_core_media_proxy_remote_addr(session, NULL);

		memset(flags, 0, sizeof(flags));
		flags[SWITCH_RTP_FLAG_DATAWAIT]++;
		flags[SWITCH_RTP_FLAG_PROXY_MEDIA]++;

		if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_WEBRTC) &&
			!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val))) {
			flags[SWITCH_RTP_FLAG_AUTOADJ]++;
		}
		timer_name = NULL;

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "PROXY AUDIO RTP [%s] %s:%d->%s:%d codec: %u ms: %d\n",
						  switch_channel_get_name(session->channel),
						  a_engine->codec_params.remote_sdp_ip,
						  a_engine->codec_params.remote_sdp_port,
						  a_engine->codec_params.remote_sdp_ip,
						  a_engine->codec_params.remote_sdp_port, a_engine->codec_params.agreed_pt, a_engine->read_impl.microseconds_per_packet / 1000);

		if (switch_rtp_ready(a_engine->rtp_session)) {
			switch_rtp_set_default_payload(a_engine->rtp_session, a_engine->codec_params.agreed_pt);
		}

	} else {
		timer_name = smh->mparams->timer_name;

		if ((var = switch_channel_get_variable(session->channel, "rtp_timer_name"))) {
			timer_name = (char *) var;
		}
	}

	if (switch_channel_up(session->channel)) {
		a_engine->rtp_session = switch_rtp_new(a_engine->codec_params.local_sdp_ip,
											   a_engine->codec_params.local_sdp_port,
											   a_engine->codec_params.remote_sdp_ip,
											   a_engine->codec_params.remote_sdp_port,
											   a_engine->codec_params.agreed_pt,
											   a_engine->read_impl.samples_per_packet,
											   a_engine->codec_params.codec_ms * 1000,
											   flags, timer_name, &err, switch_core_session_get_pool(session));
	}

	if (switch_rtp_ready(a_engine->rtp_session)) {
		uint8_t vad_in = (smh->mparams->vflags & VAD_IN);
		uint8_t vad_out = (smh->mparams->vflags & VAD_OUT);
		uint8_t inb = switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND;
		const char *ssrc;

		//switch_core_media_set_rtp_session(session, SWITCH_MEDIA_TYPE_AUDIO, a_engine->rtp_session);

		if ((ssrc = switch_channel_get_variable(session->channel, "rtp_use_ssrc"))) {
			uint32_t ssrc_ul = (uint32_t) strtoul(ssrc, NULL, 10);
			switch_rtp_set_ssrc(a_engine->rtp_session, ssrc_ul);
			a_engine->ssrc = ssrc_ul;
		} else {
			switch_rtp_set_ssrc(a_engine->rtp_session, a_engine->ssrc);
		}

		if (a_engine->remote_ssrc) {
			switch_rtp_set_remote_ssrc(a_engine->rtp_session, a_engine->remote_ssrc);
		}

		switch_channel_set_flag(session->channel, CF_FS_RTP);

		switch_channel_set_variable_printf(session->channel, "rtp_use_pt", "%d", a_engine->codec_params.agreed_pt);

		if ((val = switch_channel_get_variable(session->channel, "rtp_enable_vad_in")) && switch_true(val)) {
			vad_in = 1;
		}
		if ((val = switch_channel_get_variable(session->channel, "rtp_enable_vad_out")) && switch_true(val)) {
			vad_out = 1;
		}

		if ((val = switch_channel_get_variable(session->channel, "rtp_disable_vad_in")) && switch_true(val)) {
			vad_in = 0;
		}
		if ((val = switch_channel_get_variable(session->channel, "rtp_disable_vad_out")) && switch_true(val)) {
			vad_out = 0;
		}


		a_engine->ssrc = switch_rtp_get_ssrc(a_engine->rtp_session);
		switch_channel_set_variable_printf(session->channel, "rtp_use_ssrc", "%u", a_engine->ssrc);



		if (smh->mparams->auto_rtp_bugs & RTP_BUG_IGNORE_MARK_BIT) {
			a_engine->rtp_bugs |= RTP_BUG_IGNORE_MARK_BIT;
		}

		if ((val = switch_channel_get_variable(session->channel, "rtp_manual_rtp_bugs"))) {
			switch_core_media_parse_rtp_bugs(&a_engine->rtp_bugs, val);
		}

		switch_rtp_intentional_bugs(a_engine->rtp_session, a_engine->rtp_bugs | smh->mparams->manual_rtp_bugs);

		if ((vad_in && inb) || (vad_out && !inb)) {
			switch_rtp_enable_vad(a_engine->rtp_session, session, &a_engine->read_codec, SWITCH_VAD_FLAG_TALKING | SWITCH_VAD_FLAG_EVENTS_TALK | SWITCH_VAD_FLAG_EVENTS_NOTALK);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "AUDIO RTP Engage VAD for %s ( %s %s )\n",
							  switch_channel_get_name(switch_core_session_get_channel(session)), vad_in ? "in" : "", vad_out ? "out" : "");
		}

		
		if (a_engine->ice_in.cands[a_engine->ice_in.chosen[0]][0].ready) {
			
			gen_ice(session, SWITCH_MEDIA_TYPE_AUDIO, NULL, 0);

			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating Audio ICE\n");

			switch_rtp_activate_ice(a_engine->rtp_session, 
									a_engine->ice_in.ufrag,
									a_engine->ice_out.ufrag,
									a_engine->ice_out.pwd,
									a_engine->ice_in.pwd,
									IPR_RTP,
#ifdef GOOGLE_ICE
									ICE_GOOGLE_JINGLE,
									NULL
#else
									switch_channel_direction(session->channel) == 
									SWITCH_CALL_DIRECTION_OUTBOUND ? ICE_VANILLA : (ICE_VANILLA | ICE_CONTROLLED),
									&a_engine->ice_in
#endif
									);

		
			
		}



		if ((val = switch_channel_get_variable(session->channel, "rtcp_audio_interval_msec")) || (val = smh->mparams->rtcp_audio_interval_msec)) {
			const char *rport = switch_channel_get_variable(session->channel, "rtp_remote_audio_rtcp_port");
			switch_port_t remote_rtcp_port = a_engine->remote_rtcp_port;

			if (!remote_rtcp_port && rport) {
				remote_rtcp_port = (switch_port_t)atoi(rport);
			}
			
			if (!strcasecmp(val, "passthru")) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating RTCP PASSTHRU PORT %d\n", remote_rtcp_port);
				switch_rtp_activate_rtcp(a_engine->rtp_session, -1, remote_rtcp_port, a_engine->rtcp_mux > 0);
			} else {
				int interval = atoi(val);
				if (interval < 100 || interval > 500000) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
									  "Invalid rtcp interval spec [%d] must be between 100 and 500000\n", interval);
					interval = 10000;
				}

				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating RTCP PORT %d\n", remote_rtcp_port);
				switch_rtp_activate_rtcp(a_engine->rtp_session, interval, remote_rtcp_port, a_engine->rtcp_mux > 0);
				
			}

			if (a_engine->ice_in.cands[a_engine->ice_in.chosen[1]][1].ready) {
				if (!strcmp(a_engine->ice_in.cands[a_engine->ice_in.chosen[1]][1].con_addr, a_engine->ice_in.cands[a_engine->ice_in.chosen[0]][0].con_addr)
					&& a_engine->ice_in.cands[a_engine->ice_in.chosen[1]][1].con_port == a_engine->ice_in.cands[a_engine->ice_in.chosen[0]][0].con_port) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Skipping RTCP ICE (Same as RTP)\n");
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating RTCP ICE\n");
				
					switch_rtp_activate_ice(a_engine->rtp_session, 
											a_engine->ice_in.ufrag,
											a_engine->ice_out.ufrag,
											a_engine->ice_out.pwd,
											a_engine->ice_in.pwd,
											IPR_RTCP,
#ifdef GOOGLE_ICE
											ICE_GOOGLE_JINGLE,
											NULL
#else
											switch_channel_direction(session->channel) == 
											SWITCH_CALL_DIRECTION_OUTBOUND ? ICE_VANILLA : (ICE_VANILLA | ICE_CONTROLLED),
											&a_engine->ice_in
#endif
										);
				}
				
			}
		}

		if (!zstr(a_engine->local_dtls_fingerprint.str) && switch_rtp_has_dtls() && dtls_ok(smh->session)) {
			dtls_type_t xtype, dtype = switch_channel_direction(smh->session->channel) == SWITCH_CALL_DIRECTION_INBOUND ? DTLS_TYPE_CLIENT : DTLS_TYPE_SERVER;


			xtype = DTLS_TYPE_RTP;
			if (a_engine->rtcp_mux > 0) xtype |= DTLS_TYPE_RTCP;
		
			switch_rtp_add_dtls(a_engine->rtp_session, &a_engine->local_dtls_fingerprint, &a_engine->remote_dtls_fingerprint, dtype | xtype);

			if (a_engine->rtcp_mux < 1) {
				xtype = DTLS_TYPE_RTCP;
				switch_rtp_add_dtls(a_engine->rtp_session, &a_engine->local_dtls_fingerprint, &a_engine->remote_dtls_fingerprint, dtype | xtype);
			}

		}



		if ((val = switch_channel_get_variable(session->channel, "jitterbuffer_msec")) || (val = smh->mparams->jb_msec)) {
			int jb_msec = atoi(val);
			int maxlen = 0, max_drift = 0;
			char *p, *q;
			
			if ((p = strchr(val, ':'))) {
				p++;
				maxlen = atoi(p);
				if ((q = strchr(p, ':'))) {
					q++;
					max_drift = abs(atoi(q));
				}
			}

			if (jb_msec < 20 || jb_msec > 10000) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
								  "Invalid Jitterbuffer spec [%d] must be between 20 and 10000\n", jb_msec);
			} else {
				int qlen, maxqlen = 50;
				
				qlen = jb_msec / (a_engine->read_impl.microseconds_per_packet / 1000);

				if (qlen < 1) {
					qlen = 3;
				}

				if (maxlen) {
					maxqlen = maxlen / (a_engine->read_impl.microseconds_per_packet / 1000);
				}

				if (maxqlen < qlen) {
					maxqlen = qlen * 5;
				}
				if (switch_rtp_activate_jitter_buffer(a_engine->rtp_session, qlen, maxqlen,
													  a_engine->read_impl.samples_per_packet, 
													  a_engine->read_impl.samples_per_second, max_drift) == SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), 
									  SWITCH_LOG_DEBUG, "Setting Jitterbuffer to %dms (%d frames)\n", jb_msec, qlen);
					switch_channel_set_flag(session->channel, CF_JITTERBUFFER);
					if (!switch_false(switch_channel_get_variable(session->channel, "rtp_jitter_buffer_plc"))) {
						switch_channel_set_flag(session->channel, CF_JITTERBUFFER_PLC);
					}
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), 
									  SWITCH_LOG_WARNING, "Error Setting Jitterbuffer to %dms (%d frames)\n", jb_msec, qlen);
				}
				
			}
		}

		if ((val = switch_channel_get_variable(session->channel, "params->rtp_timeout_sec"))) {
			int v = atoi(val);
			if (v >= 0) {
				smh->mparams->rtp_timeout_sec = v;
			}
		}

		if ((val = switch_channel_get_variable(session->channel, "params->rtp_hold_timeout_sec"))) {
			int v = atoi(val);
			if (v >= 0) {
				smh->mparams->rtp_hold_timeout_sec = v;
			}
		}

		if (smh->mparams->rtp_timeout_sec) {
			a_engine->max_missed_packets = (a_engine->read_impl.samples_per_second * smh->mparams->rtp_timeout_sec) / a_engine->read_impl.samples_per_packet;

			switch_rtp_set_max_missed_packets(a_engine->rtp_session, a_engine->max_missed_packets);
			if (!smh->mparams->rtp_hold_timeout_sec) {
				smh->mparams->rtp_hold_timeout_sec = smh->mparams->rtp_timeout_sec * 10;
			}
		}

		if (smh->mparams->rtp_hold_timeout_sec) {
			a_engine->max_missed_hold_packets = (a_engine->read_impl.samples_per_second * smh->mparams->rtp_hold_timeout_sec) / a_engine->read_impl.samples_per_packet;
		}

		if (smh->mparams->te) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set 2833 dtmf send payload to %u\n", smh->mparams->te);
			switch_rtp_set_telephony_event(a_engine->rtp_session, smh->mparams->te);
			switch_channel_set_variable_printf(session->channel, "rtp_2833_send_payload", "%d", smh->mparams->te);
		}

		if (smh->mparams->recv_te) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set 2833 dtmf receive payload to %u\n", smh->mparams->recv_te);
			switch_rtp_set_telephony_recv_event(a_engine->rtp_session, smh->mparams->recv_te);
			switch_channel_set_variable_printf(session->channel, "rtp_2833_recv_payload", "%d", smh->mparams->recv_te);
		}

		if (a_engine->codec_params.recv_pt != a_engine->codec_params.agreed_pt) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
							  "%s Set audio receive payload to %u\n", switch_channel_get_name(session->channel), a_engine->codec_params.recv_pt);

			switch_rtp_set_recv_pt(a_engine->rtp_session, a_engine->codec_params.recv_pt);
		}

		if (switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) ||
			((val = switch_channel_get_variable(session->channel, "supress_cng")) && switch_true(val)) ||
			((val = switch_channel_get_variable(session->channel, "suppress_cng")) && switch_true(val))) {
			smh->mparams->cng_pt = 0;
		}

		if (((val = switch_channel_get_variable(session->channel, "rtp_digit_delay")))) {
			int delayi = atoi(val);
			if (delayi < 0) delayi = 0;
			smh->mparams->dtmf_delay = (uint32_t) delayi;
		}


		if (smh->mparams->dtmf_delay) {
			switch_rtp_set_interdigit_delay(a_engine->rtp_session, smh->mparams->dtmf_delay);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
							  "%s Set rtp dtmf delay to %u\n", switch_channel_get_name(session->channel), smh->mparams->dtmf_delay);
			
		}

		if (smh->mparams->cng_pt && !switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Set comfort noise payload to %u\n", smh->mparams->cng_pt);
			switch_rtp_set_cng_pt(a_engine->rtp_session, smh->mparams->cng_pt);
		}

		switch_core_session_apply_crypto(session, SWITCH_MEDIA_TYPE_AUDIO);

		switch_snprintf(tmp, sizeof(tmp), "%d", a_engine->codec_params.remote_sdp_port);
		switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE, a_engine->codec_params.remote_sdp_ip);
		switch_channel_set_variable(session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE, tmp);


		if (switch_channel_test_flag(session->channel, CF_ZRTP_PASSTHRU)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating ZRTP PROXY MODE\n");
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Disable NOTIMER_DURING_BRIDGE\n");
			switch_channel_clear_flag(session->channel, CF_NOTIMER_DURING_BRIDGE);
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Activating audio UDPTL mode\n");
			switch_rtp_udptl_mode(a_engine->rtp_session);
		}


		
	

	
	video:
	
		if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
			switch_core_media_check_video_codecs(session);
		}
		
		if (switch_channel_test_flag(session->channel, CF_VIDEO_POSSIBLE) && v_engine->codec_params.rm_encoding && v_engine->codec_params.remote_sdp_port) {
			/******************************************************************************************/
			if (v_engine->rtp_session && switch_channel_test_flag(session->channel, CF_REINVITE)) {
				//const char *ip = switch_channel_get_variable(session->channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE);
				//const char *port = switch_channel_get_variable(session->channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE);
				char *remote_host = switch_rtp_get_remote_host(v_engine->rtp_session);
				switch_port_t remote_port = switch_rtp_get_remote_port(v_engine->rtp_session);
				


				if (remote_host && remote_port && !strcmp(remote_host, v_engine->codec_params.remote_sdp_ip) && remote_port == v_engine->codec_params.remote_sdp_port) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Video params are unchanged for %s.\n",
									  switch_channel_get_name(session->channel));
					goto video_up;
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Video params changed for %s from %s:%d to %s:%d\n",
									  switch_channel_get_name(session->channel),
									  remote_host, remote_port, v_engine->codec_params.remote_sdp_ip, v_engine->codec_params.remote_sdp_port);
				}
			}

			if (!switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
				if (switch_rtp_ready(v_engine->rtp_session)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
									  "VIDEO RTP [%s] %s port %d -> %s port %d codec: %u ms: %d\n", switch_channel_get_name(session->channel),
									  a_engine->codec_params.remote_sdp_ip, v_engine->codec_params.local_sdp_port, v_engine->codec_params.remote_sdp_ip,
									  v_engine->codec_params.remote_sdp_port, v_engine->codec_params.agreed_pt, 
									  a_engine->read_impl.microseconds_per_packet / 1000);

				
					switch_rtp_set_default_payload(v_engine->rtp_session, v_engine->codec_params.agreed_pt);
				}
			}
			
			switch_snprintf(tmp, sizeof(tmp), "%d", v_engine->codec_params.local_sdp_port);
			switch_channel_set_variable(session->channel, SWITCH_LOCAL_VIDEO_IP_VARIABLE, a_engine->codec_params.adv_sdp_ip);
			switch_channel_set_variable(session->channel, SWITCH_LOCAL_VIDEO_PORT_VARIABLE, tmp);


			if (v_engine->rtp_session && switch_channel_test_flag(session->channel, CF_REINVITE)) {
				const char *rport = NULL;
				switch_port_t remote_rtcp_port = v_engine->remote_rtcp_port;

				switch_channel_clear_flag(session->channel, CF_REINVITE);

				if (!remote_rtcp_port) {
					if ((rport = switch_channel_get_variable(session->channel, "rtp_remote_video_rtcp_port"))) {
						remote_rtcp_port = (switch_port_t)atoi(rport);
					}
				}
				
				if (switch_rtp_set_remote_address
					(v_engine->rtp_session, v_engine->codec_params.remote_sdp_ip, v_engine->codec_params.remote_sdp_port, remote_rtcp_port, SWITCH_TRUE,
					 &err) != SWITCH_STATUS_SUCCESS) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "VIDEO RTP REPORTS ERROR: [%s]\n", err);
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "VIDEO RTP CHANGING DEST TO: [%s:%d]\n",
									  v_engine->codec_params.remote_sdp_ip, v_engine->codec_params.remote_sdp_port);
					if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_WEBRTC) &&
						!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val))) {
						/* Reactivate the NAT buster flag. */
						switch_rtp_set_flag(v_engine->rtp_session, SWITCH_RTP_FLAG_AUTOADJ);
					}

				}
				goto video_up;
			}

			if (switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
				switch_core_media_proxy_remote_addr(session, NULL);

				memset(flags, 0, sizeof(flags));
				flags[SWITCH_RTP_FLAG_PROXY_MEDIA]++;
				flags[SWITCH_RTP_FLAG_DATAWAIT]++;

				if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_WEBRTC) &&
					!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val))) {
					flags[SWITCH_RTP_FLAG_AUTOADJ]++;
				}
				timer_name = NULL;

				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
								  "PROXY VIDEO RTP [%s] %s:%d->%s:%d codec: %u ms: %d\n",
								  switch_channel_get_name(session->channel),
								  a_engine->codec_params.remote_sdp_ip,
								  v_engine->codec_params.local_sdp_port,
								  v_engine->codec_params.remote_sdp_ip,
								  v_engine->codec_params.remote_sdp_port, v_engine->codec_params.agreed_pt, v_engine->read_impl.microseconds_per_packet / 1000);

				if (switch_rtp_ready(v_engine->rtp_session)) {
					switch_rtp_set_default_payload(v_engine->rtp_session, v_engine->codec_params.agreed_pt);
				}
			} else {
				timer_name = smh->mparams->timer_name;

				if ((var = switch_channel_get_variable(session->channel, "rtp_timer_name"))) {
					timer_name = (char *) var;
				}
			}

			/******************************************************************************************/

			if (v_engine->rtp_session) {
				goto video_up;
			}


			if (!v_engine->codec_params.local_sdp_port) {
				switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_VIDEO, 1);
			}

			memset(flags, 0, sizeof(flags));
			flags[SWITCH_RTP_FLAG_DATAWAIT]++;
			flags[SWITCH_RTP_FLAG_RAW_WRITE]++;

			if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_PROXY_MODE) &&
				!((val = switch_channel_get_variable(session->channel, "disable_rtp_auto_adjust")) && switch_true(val)) && 
				!switch_channel_test_flag(session->channel, CF_WEBRTC)) {
				flags[SWITCH_RTP_FLAG_AUTOADJ]++;				
			}

			if (switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
				flags[SWITCH_RTP_FLAG_PROXY_MEDIA]++;
			}
			switch_core_media_set_video_codec(session, 0);

			flags[SWITCH_RTP_FLAG_USE_TIMER] = 0;
			flags[SWITCH_RTP_FLAG_NOBLOCK] = 0;
			flags[SWITCH_RTP_FLAG_VIDEO]++;

			v_engine->rtp_session = switch_rtp_new(a_engine->codec_params.local_sdp_ip,
														 v_engine->codec_params.local_sdp_port,
														 v_engine->codec_params.remote_sdp_ip,
														 v_engine->codec_params.remote_sdp_port,
														 v_engine->codec_params.agreed_pt,
														 1, 90000, flags, NULL, &err, switch_core_session_get_pool(session));


			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%sVIDEO RTP [%s] %s:%d->%s:%d codec: %u ms: %d [%s]\n",
							  switch_channel_test_flag(session->channel, CF_PROXY_MEDIA) ? "PROXY " : "",
							  switch_channel_get_name(session->channel),
							  a_engine->codec_params.remote_sdp_ip,
							  v_engine->codec_params.local_sdp_port,
							  v_engine->codec_params.remote_sdp_ip,
							  v_engine->codec_params.remote_sdp_port, v_engine->codec_params.agreed_pt,
							  0, switch_rtp_ready(v_engine->rtp_session) ? "SUCCESS" : err);


			if (switch_rtp_ready(v_engine->rtp_session)) {
				switch_threadattr_t *thd_attr = NULL;
				switch_memory_pool_t *pool = switch_core_session_get_pool(session);

				switch_rtp_set_default_payload(v_engine->rtp_session, v_engine->codec_params.agreed_pt);
				v_engine->mh.session = session;
				switch_threadattr_create(&thd_attr, pool);
				switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

				switch_thread_cond_create(&v_engine->mh.cond, pool);
				switch_mutex_init(&v_engine->mh.cond_mutex, SWITCH_MUTEX_NESTED, pool);
				switch_thread_create(&v_engine->media_thread, thd_attr, video_helper_thread, &v_engine->mh, switch_core_session_get_pool(session));
			}

			if (switch_rtp_ready(v_engine->rtp_session)) {
				const char *ssrc;
				switch_channel_set_flag(session->channel, CF_VIDEO);
				if ((ssrc = switch_channel_get_variable(session->channel, "rtp_use_video_ssrc"))) {
					uint32_t ssrc_ul = (uint32_t) strtoul(ssrc, NULL, 10);
					switch_rtp_set_ssrc(v_engine->rtp_session, ssrc_ul);
					v_engine->ssrc = ssrc_ul;
				} else {
					switch_rtp_set_ssrc(v_engine->rtp_session, v_engine->ssrc);
				}
				
				if (v_engine->remote_ssrc) {
					switch_rtp_set_remote_ssrc(v_engine->rtp_session, v_engine->remote_ssrc);
				}

				if (v_engine->ice_in.cands[v_engine->ice_in.chosen[0]][0].ready) {
					
					gen_ice(session, SWITCH_MEDIA_TYPE_VIDEO, NULL, 0);

					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating Video ICE\n");
						
					switch_rtp_activate_ice(v_engine->rtp_session, 
											v_engine->ice_in.ufrag,
											v_engine->ice_out.ufrag,
											v_engine->ice_out.pwd,
											v_engine->ice_in.pwd,
											IPR_RTP,
#ifdef GOOGLE_ICE
											ICE_GOOGLE_JINGLE,
											NULL
#else
											switch_channel_direction(session->channel) == 
											SWITCH_CALL_DIRECTION_OUTBOUND ? ICE_VANILLA : (ICE_VANILLA | ICE_CONTROLLED),
											&v_engine->ice_in
#endif
											);
						
					
				}

				if ((val = switch_channel_get_variable(session->channel, "rtcp_video_interval_msec")) || (val = smh->mparams->rtcp_video_interval_msec)) {
					const char *rport = switch_channel_get_variable(session->channel, "rtp_remote_video_rtcp_port");
					switch_port_t remote_port = v_engine->remote_rtcp_port;

					if (rport) {
						remote_port = (switch_port_t)atoi(rport);
					}
					if (!strcasecmp(val, "passthru")) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating VIDEO RTCP PASSTHRU PORT %d\n", remote_port);
						switch_rtp_activate_rtcp(v_engine->rtp_session, -1, remote_port, v_engine->rtcp_mux > 0);
					} else {
						int interval = atoi(val);
						if (interval < 100 || interval > 500000) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR,
											  "Invalid rtcp interval spec [%d] must be between 100 and 500000\n", interval);
						}
						interval = 10000;
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating VIDEO RTCP PORT %d mux %d\n", remote_port, v_engine->rtcp_mux);
						switch_rtp_activate_rtcp(v_engine->rtp_session, interval, remote_port, v_engine->rtcp_mux > 0);
							
					}
					

					if (v_engine->ice_in.cands[v_engine->ice_in.chosen[1]][1].ready) {

						if (!strcmp(v_engine->ice_in.cands[v_engine->ice_in.chosen[1]][1].con_addr, v_engine->ice_in.cands[v_engine->ice_in.chosen[0]][0].con_addr)
							&& v_engine->ice_in.cands[v_engine->ice_in.chosen[1]][1].con_port == v_engine->ice_in.cands[v_engine->ice_in.chosen[0]][0].con_port) {
							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Skipping VIDEO RTCP ICE (Same as VIDEO RTP)\n");
						} else {

							switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Activating VIDEO RTCP ICE\n");
							switch_rtp_activate_ice(v_engine->rtp_session, 
													v_engine->ice_in.ufrag,
													v_engine->ice_out.ufrag,
													v_engine->ice_out.pwd,
													v_engine->ice_in.pwd,
													IPR_RTCP,
#ifdef GOOGLE_ICE
													ICE_GOOGLE_JINGLE,
													NULL
#else
													switch_channel_direction(session->channel) == 
													SWITCH_CALL_DIRECTION_OUTBOUND ? ICE_VANILLA : (ICE_VANILLA | ICE_CONTROLLED),
													
													&v_engine->ice_in
#endif
													);
						
						
						
						}
				
					}
				}
				
				if (!zstr(v_engine->local_dtls_fingerprint.str) && switch_rtp_has_dtls() && dtls_ok(smh->session)) {
					dtls_type_t xtype, 
						dtype = switch_channel_direction(smh->session->channel) == SWITCH_CALL_DIRECTION_INBOUND ? DTLS_TYPE_CLIENT : DTLS_TYPE_SERVER;
					xtype = DTLS_TYPE_RTP;
					if (v_engine->rtcp_mux > 0) xtype |= DTLS_TYPE_RTCP;
					
					switch_rtp_add_dtls(v_engine->rtp_session, &v_engine->local_dtls_fingerprint, &v_engine->remote_dtls_fingerprint, dtype | xtype);
					
					if (v_engine->rtcp_mux < 1) {
						xtype = DTLS_TYPE_RTCP;
						switch_rtp_add_dtls(v_engine->rtp_session, &v_engine->local_dtls_fingerprint, &v_engine->remote_dtls_fingerprint, dtype | xtype);
					}
				}
					
					
				if ((val = switch_channel_get_variable(session->channel, "rtp_manual_video_rtp_bugs"))) {
					switch_core_media_parse_rtp_bugs(&v_engine->rtp_bugs, val);
				}
				
				switch_rtp_intentional_bugs(v_engine->rtp_session, v_engine->rtp_bugs | smh->mparams->manual_video_rtp_bugs);

				if (v_engine->codec_params.recv_pt != v_engine->codec_params.agreed_pt) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, 
									  "%s Set video receive payload to %u\n", switch_channel_get_name(session->channel), v_engine->codec_params.recv_pt);
					switch_rtp_set_recv_pt(v_engine->rtp_session, v_engine->codec_params.recv_pt);
				}

				switch_channel_set_variable_printf(session->channel, "rtp_use_video_pt", "%d", v_engine->codec_params.agreed_pt);
				v_engine->ssrc = switch_rtp_get_ssrc(v_engine->rtp_session);
				switch_channel_set_variable_printf(session->channel, "rtp_use_video_ssrc", "%u", v_engine->ssrc);

				switch_core_session_apply_crypto(session, SWITCH_MEDIA_TYPE_VIDEO);

				
				if (switch_channel_test_flag(session->channel, CF_ZRTP_PASSTHRU)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Activating video UDPTL mode\n");
					switch_rtp_udptl_mode(v_engine->rtp_session);
				}

			} else {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "VIDEO RTP REPORTS ERROR: [%s]\n", switch_str_nil(err));
				switch_channel_hangup(session->channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
				goto end;
			}
		}

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "AUDIO RTP REPORTS ERROR: [%s]\n", switch_str_nil(err));
		switch_channel_hangup(session->channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
		status = SWITCH_STATUS_FALSE;
		goto end;
	}

 video_up:

	status = SWITCH_STATUS_SUCCESS;

 end:

	switch_channel_clear_flag(session->channel, CF_REINVITE);

	switch_core_recovery_track(session);



	return status;

}

static const char *get_media_profile_name(switch_core_session_t *session, int secure)
{
	switch_assert(session);

	if (switch_channel_test_flag(session->channel, CF_WEBRTC)) {
		if (switch_channel_test_flag(session->channel, CF_WEBRTC_MOZ)) {
			return "UDP/TLS/RTP/SAVPF";
		} else {
			return "RTP/SAVPF";
		}
	}

	if (secure) {
		return "RTP/SAVP";
	}

	return "RTP/AVP";
	
}

//?
static void generate_m(switch_core_session_t *session, char *buf, size_t buflen, 
					   switch_port_t port, const char *family, const char *ip,
					   int cur_ptime, const char *append_audio, const char *sr, int use_cng, int cng_type, switch_event_t *map, int secure)
{
	int i = 0;
	int rate;
	int already_did[128] = { 0 };
	int ptime = 0, noptime = 0;
	const char *local_audio_crypto_key = switch_core_session_local_crypto_key(session, SWITCH_MEDIA_TYPE_AUDIO);
	const char *local_sdp_audio_zrtp_hash;
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];

	//switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "m=audio %d RTP/%sAVP%s", 
	//port, secure ? "S" : "", switch_channel_test_flag(session->channel, CF_WEBRTC) ? "F" : "");

	switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "m=audio %d %s", port, get_media_profile_name(session, secure));
	

	for (i = 0; i < smh->mparams->num_codecs; i++) {
		const switch_codec_implementation_t *imp = smh->codecs[i];
		int this_ptime = (imp->microseconds_per_packet / 1000);

		if (!strcasecmp(imp->iananame, "ilbc") || !strcasecmp(imp->iananame, "isac") ) {
			this_ptime = 20;
		}

		if (imp->codec_type != SWITCH_CODEC_TYPE_AUDIO) {
			continue;
		}

		if (!noptime) {
			if (!cur_ptime) {
				if (!ptime) {
					ptime = this_ptime;
				}
			} else {
				if (this_ptime != cur_ptime) {
					continue;
				}
			}
		}

		if (smh->ianacodes[i] < 128) {
			if (already_did[smh->ianacodes[i]]) {
				continue;
			}

			already_did[smh->ianacodes[i]] = 1;
		}

		
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), " %d", smh->ianacodes[i]);
	}

	if (smh->mparams->dtmf_type == DTMF_2833 && smh->mparams->te > 95) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), " %d", smh->mparams->te);
	}
		
	if (!switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) && cng_type && use_cng) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), " %d", cng_type);
	}
		
	switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "\n");


	memset(already_did, 0, sizeof(already_did));

		
	for (i = 0; i < smh->mparams->num_codecs; i++) {
		const switch_codec_implementation_t *imp = smh->codecs[i];
		char *fmtp = imp->fmtp;
		int this_ptime = imp->microseconds_per_packet / 1000;

		if (imp->codec_type != SWITCH_CODEC_TYPE_AUDIO) {
			continue;
		}

		if (!strcasecmp(imp->iananame, "ilbc") || !strcasecmp(imp->iananame, "isac")) {
			this_ptime = 20;
		}

		if (!noptime) {
			if (!cur_ptime) {
				if (!ptime) {
					ptime = this_ptime;
				}
			} else {
				if (this_ptime != cur_ptime) {
					continue;
				}
			}
		}
		
		if (smh->ianacodes[i] < 128) {
			if (already_did[smh->ianacodes[i]]) {
				continue;
			}
			
			already_did[smh->ianacodes[i]] = 1;
		}

		
		rate = imp->samples_per_second;

		if (map) {
			char key[128] = "";
			char *check = NULL;
			switch_snprintf(key, sizeof(key), "%s:%u", imp->iananame, imp->bits_per_second);

			if ((check = switch_event_get_header(map, key)) || (check = switch_event_get_header(map, imp->iananame))) {
				fmtp = check;
			}
		}
		
		if (smh->ianacodes[i] > 95 || switch_channel_test_flag(session->channel, CF_VERBOSE_SDP)) {
			int channels = get_channels(imp);

			if (channels > 1) {
				switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtpmap:%d %s/%d/%d\n", smh->ianacodes[i], imp->iananame, rate, channels);
								
			} else {
				switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtpmap:%d %s/%d\n", smh->ianacodes[i], imp->iananame, rate);
			}
		}

		if (fmtp) {
			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=fmtp:%d %s\n", smh->ianacodes[i], fmtp);
		}
	}


	if ((smh->mparams->dtmf_type == DTMF_2833 || switch_media_handle_test_media_flag(smh, SCMF_LIBERAL_DTMF) || 
		 switch_channel_test_flag(session->channel, CF_LIBERAL_DTMF)) && smh->mparams->te > 95) {

		if (switch_channel_test_flag(session->channel, CF_WEBRTC)) {
			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtpmap:%d telephone-event/8000\n", smh->mparams->te);
		} else {
			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtpmap:%d telephone-event/8000\na=fmtp:%d 0-16\n", smh->mparams->te, smh->mparams->te);
		}

	}

	if (!zstr(a_engine->local_dtls_fingerprint.type) && secure) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=fingerprint:%s %s\n", a_engine->local_dtls_fingerprint.type, 
						a_engine->local_dtls_fingerprint.str);
	}

	if (smh->mparams->rtcp_audio_interval_msec) {
		if (a_engine->rtcp_mux > 0) {
			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtcp-mux\n");
			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtcp:%d IN %s %s\n", port, family, ip);
		} else {
			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtcp:%d IN %s %s\n", port + 1, family, ip);
		}
	}

	//switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u\n", a_engine->ssrc);

	if (a_engine->ice_out.cands[0][0].ready) {
		char tmp1[11] = "";
		char tmp2[11] = "";
		uint32_t c1 = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - 1);
		uint32_t c2 = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - 2);
		//uint32_t c3 = (2^24)*126 + (2^8)*65534 + (2^0)*(256 - 1);
		//uint32_t c4 = (2^24)*126 + (2^8)*65534 + (2^0)*(256 - 2);
		ice_t *ice_out;

		tmp1[10] = '\0';
		tmp2[10] = '\0';
		switch_stun_random_string(tmp1, 10, "0123456789");
		switch_stun_random_string(tmp2, 10, "0123456789");

		gen_ice(session, SWITCH_MEDIA_TYPE_AUDIO, NULL, 0);

		ice_out = &a_engine->ice_out;

		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ssrc:%u cname:%s\n", a_engine->ssrc, smh->cname);
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ssrc:%u msid:%s a0\n", a_engine->ssrc, smh->msid);
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ssrc:%u mslabel:%s\n", a_engine->ssrc, smh->msid);
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ssrc:%u label:%sa0\n", a_engine->ssrc, smh->msid);


		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ice-ufrag:%s\n", ice_out->ufrag);
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ice-pwd:%s\n", ice_out->pwd);


		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=candidate:%s 1 %s %u %s %d typ host generation 0\n", 
						tmp1, ice_out->cands[0][0].transport, c1,
						ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port
						);

		if (!zstr(a_engine->codec_params.local_sdp_ip) && !zstr(ice_out->cands[0][0].con_addr) && 
			strcmp(a_engine->codec_params.local_sdp_ip, ice_out->cands[0][0].con_addr)
			&& a_engine->codec_params.local_sdp_port != ice_out->cands[0][0].con_port) {

			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=candidate:%s 1 %s %u %s %d typ srflx raddr %s rport %d generation 0\n", 
							tmp2, ice_out->cands[0][0].transport, c2,
							ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port,
							a_engine->codec_params.local_sdp_ip, a_engine->codec_params.local_sdp_port
							);
		}

		if (a_engine->rtcp_mux < 1 || switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
			

			switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=candidate:%s 2 %s %u %s %d typ host generation 0\n", 
							tmp1, ice_out->cands[0][0].transport, c1,
							ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port + (a_engine->rtcp_mux > 0 ? 0 : 1)
							);
			
			if (!zstr(a_engine->codec_params.local_sdp_ip) && !zstr(ice_out->cands[0][1].con_addr) && 
				strcmp(a_engine->codec_params.local_sdp_ip, ice_out->cands[0][1].con_addr)
				&& a_engine->codec_params.local_sdp_port != ice_out->cands[0][1].con_port) {
				
				switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=candidate:%s 2 %s %u %s %d typ srflx raddr %s rport %d generation 0\n", 
								tmp2, ice_out->cands[0][0].transport, c2,
								ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port + (a_engine->rtcp_mux > 0 ? 0 : 1),
								a_engine->codec_params.local_sdp_ip, a_engine->codec_params.local_sdp_port + (a_engine->rtcp_mux > 0 ? 0 : 1)
								);
			}
		}

			
				
#ifdef GOOGLE_ICE
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ice-options:google-ice\n");
#endif
	}


	if (secure && !zstr(local_audio_crypto_key)) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=crypto:%s\n", local_audio_crypto_key);
		//switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "a=encryption:optional\n");
	}

	if (!cng_type) {
		//switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=rtpmap:%d CN/8000\n", cng_type);
		//} else {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=silenceSupp:off - - - -\n");
	}

	if (append_audio) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "%s%s", append_audio, end_of(append_audio) == '\n' ? "" : "\n");
	}

	if (!cur_ptime) {
		cur_ptime = ptime;
	}
	
	if (!noptime && cur_ptime) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=ptime:%d\n", cur_ptime);
	}

	local_sdp_audio_zrtp_hash = switch_core_media_get_zrtp_hash(session, SWITCH_MEDIA_TYPE_AUDIO, SWITCH_TRUE);

	if (local_sdp_audio_zrtp_hash) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Adding audio a=zrtp-hash:%s\n", local_sdp_audio_zrtp_hash);
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=zrtp-hash:%s\n", local_sdp_audio_zrtp_hash);
	}

	if (!zstr(sr)) {
		switch_snprintf(buf + strlen(buf), buflen - strlen(buf), "a=%s\n", sr);
	}
}

//?
SWITCH_DECLARE(void) switch_core_media_check_dtmf_type(switch_core_session_t *session) 
{
	const char *val;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if ((val = switch_channel_get_variable(session->channel, "dtmf_type"))) {
		if (!strcasecmp(val, "rfc2833")) {
			smh->mparams->dtmf_type = DTMF_2833;
		} else if (!strcasecmp(val, "info")) {
			smh->mparams->dtmf_type = DTMF_INFO;
		} else if (!strcasecmp(val, "none")) {
			smh->mparams->dtmf_type = DTMF_NONE;
		}
	}
}

//?
switch_status_t switch_core_media_sdp_map(const char *r_sdp, switch_event_t **fmtp, switch_event_t **pt)
{
	sdp_media_t *m;
	sdp_parser_t *parser = NULL;
	sdp_session_t *sdp;

	if (!(parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
		return SWITCH_STATUS_FALSE;
	}

	if (!(sdp = sdp_session(parser))) {
		sdp_parser_free(parser);
		return SWITCH_STATUS_FALSE;
	}

	switch_event_create(&(*fmtp), SWITCH_EVENT_REQUEST_PARAMS);
	switch_event_create(&(*pt), SWITCH_EVENT_REQUEST_PARAMS);

	for (m = sdp->sdp_media; m; m = m->m_next) {
		if (m->m_proto == sdp_proto_rtp) {
			sdp_rtpmap_t *map;
			
			for (map = m->m_rtpmaps; map; map = map->rm_next) {
				if (map->rm_encoding) {
					char buf[25] = "";
					char key[128] = "";
					char *br = NULL;

					if (map->rm_fmtp) {
						if ((br = strstr(map->rm_fmtp, "bitrate="))) {
							br += 8;
						}
					}

					switch_snprintf(buf, sizeof(buf), "%d", map->rm_pt);

					if (br) {
						switch_snprintf(key, sizeof(key), "%s:%s", map->rm_encoding, br);
					} else {
						switch_snprintf(key, sizeof(key), "%s", map->rm_encoding);
					}
					
					switch_event_add_header_string(*pt, SWITCH_STACK_BOTTOM, key, buf);

					if (map->rm_fmtp) {
						switch_event_add_header_string(*fmtp, SWITCH_STACK_BOTTOM, key, map->rm_fmtp);
					}
				}
			}
		}
	}
	
	sdp_parser_free(parser);

	return SWITCH_STATUS_SUCCESS;
	
}

//?
SWITCH_DECLARE(void)switch_core_media_set_local_sdp(switch_core_session_t *session, const char *sdp_str, switch_bool_t dup)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (smh->mutex) switch_mutex_lock(smh->mutex);
	smh->mparams->local_sdp_str = dup ? switch_core_session_strdup(session, sdp_str) : (char *) sdp_str;
	switch_channel_set_variable(session->channel, "rtp_local_sdp_str", smh->mparams->local_sdp_str);
	if (smh->mutex) switch_mutex_unlock(smh->mutex);
}


//?
#define SDPBUFLEN 65536
SWITCH_DECLARE(void) switch_core_media_gen_local_sdp(switch_core_session_t *session, const char *ip, switch_port_t port, const char *sr, int force)
{
	char *buf;
	int ptime = 0;
	uint32_t rate = 0;
	uint32_t v_port;
	int use_cng = 1;
	const char *val;
	const char *family;
	const char *pass_fmtp = switch_channel_get_variable(session->channel, "rtp_video_fmtp");
	const char *ov_fmtp = switch_channel_get_variable(session->channel, "rtp_force_video_fmtp");
	const char *append_audio = switch_channel_get_variable(session->channel, "rtp_append_audio_sdp");
	const char *append_video = switch_channel_get_variable(session->channel, "rtp_append_video_sdp");
	char srbuf[128] = "";
	const char *var_val;
	const char *username;
	const char *fmtp_out;
	const char *fmtp_out_var = switch_channel_get_variable(session->channel, "rtp_force_audio_fmtp");
	switch_event_t *map = NULL, *ptmap = NULL;
	const char *b_sdp = NULL;
	const char *local_audio_crypto_key = switch_core_session_local_crypto_key(session, SWITCH_MEDIA_TYPE_AUDIO);
	const char *local_sdp_audio_zrtp_hash = switch_core_media_get_zrtp_hash(session, SWITCH_MEDIA_TYPE_AUDIO, SWITCH_TRUE);
	const char *local_sdp_video_zrtp_hash = switch_core_media_get_zrtp_hash(session, SWITCH_MEDIA_TYPE_VIDEO, SWITCH_TRUE);
	const char *tmp;
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;
	ice_t *ice_out;
	int vp8 = 0;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	if (dtls_ok(session) && (tmp = switch_channel_get_variable(smh->session->channel, "webrtc_enable_dtls")) && switch_false(tmp)) {
		switch_channel_clear_flag(smh->session->channel, CF_DTLS_OK);
		switch_channel_clear_flag(smh->session->channel, CF_DTLS);
	}

	if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {
		if (!switch_channel_test_flag(session->channel, CF_WEBRTC) && 
			switch_true(switch_channel_get_variable(session->channel, "media_webrtc"))) {
			switch_channel_set_flag(session->channel, CF_WEBRTC);
			switch_channel_set_flag(session->channel, CF_ICE);
			smh->mparams->rtcp_audio_interval_msec = "5000";
			smh->mparams->rtcp_video_interval_msec = "5000";
		}

		if ( switch_rtp_has_dtls() && dtls_ok(session)) {
			if (switch_channel_test_flag(session->channel, CF_WEBRTC) ||
				switch_true(switch_channel_get_variable(smh->session->channel, "rtp_use_dtls"))) {
				switch_channel_set_flag(smh->session->channel, CF_DTLS);
				switch_channel_set_flag(smh->session->channel, CF_SECURE);
				generate_local_fingerprint(smh, SWITCH_MEDIA_TYPE_AUDIO);
			}
		}
		
		switch_core_session_check_outgoing_crypto(session, "rtp_secure_media");
		local_audio_crypto_key = switch_core_session_local_crypto_key(session, SWITCH_MEDIA_TYPE_AUDIO);

	} else {
		if (switch_channel_test_flag(smh->session->channel, CF_DTLS)) {
			local_audio_crypto_key = NULL;
		}
	}

	fmtp_out = a_engine->codec_params.fmtp_out;
	username = smh->mparams->sdp_username;


	switch_zmalloc(buf, SDPBUFLEN);
	
	switch_core_media_check_dtmf_type(session);

	if (switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) ||
		((val = switch_channel_get_variable(session->channel, "supress_cng")) && switch_true(val)) ||
		((val = switch_channel_get_variable(session->channel, "suppress_cng")) && switch_true(val))) {
		use_cng = 0;
		smh->mparams->cng_pt = 0;
	}

	if (!smh->payload_space) {
		int i;

		smh->payload_space = 98;

		for (i = 0; i < smh->mparams->num_codecs; i++) {
			const switch_codec_implementation_t *imp = smh->codecs[i];

			smh->ianacodes[i] = imp->ianacode;
			
			if (smh->ianacodes[i] > 64) {
				if (smh->mparams->dtmf_type == DTMF_2833 && smh->mparams->te > 95 && smh->mparams->te == smh->payload_space) {
					smh->payload_space++;
				}
				if (!switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) &&
					smh->mparams->cng_pt && use_cng  && smh->mparams->cng_pt == smh->payload_space) {
					smh->payload_space++;
				}
				smh->ianacodes[i] = (switch_payload_t)smh->payload_space++;
			}
		}
	}

	if (fmtp_out_var) {
		fmtp_out = fmtp_out_var;
	}

	if ((val = switch_channel_get_variable(session->channel, "verbose_sdp")) && switch_true(val)) {
		switch_channel_set_flag(session->channel, CF_VERBOSE_SDP);
	}

	if (!force && !ip && zstr(sr)
		&& (switch_channel_test_flag(session->channel, CF_PROXY_MODE) || switch_channel_test_flag(session->channel, CF_PROXY_MEDIA))) {
		switch_safe_free(buf);
		return;
	}

	if (!ip) {
		if (!(ip = a_engine->codec_params.adv_sdp_ip)) {
			ip = a_engine->codec_params.proxy_sdp_ip;
		}
	}

	if (!ip) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "%s NO IP!\n", switch_channel_get_name(session->channel));
		switch_safe_free(buf);
		return;
	}

	if (!port) {
		if (!(port = a_engine->codec_params.adv_sdp_port)) {
			port = a_engine->codec_params.proxy_sdp_port;
		}
	}

	if (!port) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "%s NO PORT!\n", switch_channel_get_name(session->channel));
		switch_safe_free(buf);
		return;
	}

	if (!a_engine->codec_params.rm_encoding && (b_sdp = switch_channel_get_variable(session->channel, SWITCH_B_SDP_VARIABLE))) {
		switch_core_media_sdp_map(b_sdp, &map, &ptmap);
	}

	if (zstr(sr)) {
		if ((var_val = switch_channel_get_variable(session->channel, "media_audio_mode"))) {
			sr = var_val;
		} else {
			sr = "sendrecv";
		}
	}

	if (!smh->owner_id) {
		smh->owner_id = (uint32_t) switch_epoch_time_now(NULL) - port;
	}

	if (!smh->session_id) {
		smh->session_id = smh->owner_id;
	}

	if (switch_true(switch_channel_get_variable_dup(session->channel, "drop_dtmf", SWITCH_FALSE, -1))) {
		switch_channel_set_flag(session->channel, CF_DROP_DTMF);
	}

	smh->session_id++;
	
	if ((smh->mparams->ndlb & SM_NDLB_SENDRECV_IN_SESSION) ||
		((var_val = switch_channel_get_variable(session->channel, "ndlb_sendrecv_in_session")) && switch_true(var_val))) {
		if (!zstr(sr)) {
			switch_snprintf(srbuf, sizeof(srbuf), "a=%s\n", sr);
		}
		sr = NULL;
	}

	family = strchr(ip, ':') ? "IP6" : "IP4";
	switch_snprintf(buf, SDPBUFLEN,
					"v=0\n"
					"o=%s %010u %010u IN %s %s\n"
					"s=%s\n"
					"c=IN %s %s\n" 
					"t=0 0\n"
					"%s",
					username, smh->owner_id, smh->session_id, family, ip, username, family, ip, srbuf);


	if (switch_channel_test_flag(smh->session->channel, CF_ICE)) {
		gen_ice(session, SWITCH_MEDIA_TYPE_AUDIO, ip, port);
		switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=msid-semantic: WMS %s\n", smh->msid);
	}


	if (a_engine->codec_params.rm_encoding) {
		/*
		switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "m=audio %d RTP/%sAVP%s", 
						port, ((!zstr(local_audio_crypto_key) || switch_channel_test_flag(session->channel, CF_DTLS)) && 
							   switch_channel_test_flag(session->channel, CF_SECURE)) ? "S" : "",
						switch_channel_test_flag(session->channel, CF_WEBRTC) ? "F" : "");
		*/

		switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "m=audio %d %s", port, 
						get_media_profile_name(session, 
											   ((!zstr(local_audio_crypto_key) || switch_channel_test_flag(session->channel, CF_DTLS)) &&
												switch_channel_test_flag(session->channel, CF_SECURE))
											   ));
		
		
		switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), " %d", a_engine->codec_params.pt);

		if ((smh->mparams->dtmf_type == DTMF_2833 || switch_media_handle_test_media_flag(smh, SCMF_LIBERAL_DTMF) || 
			 switch_channel_test_flag(session->channel, CF_LIBERAL_DTMF)) && smh->mparams->te > 95) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), " %d", smh->mparams->te);
		}
		
		if (!switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) && smh->mparams->cng_pt && use_cng) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), " %d", smh->mparams->cng_pt);
		}
		
		switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "\n");


		rate = a_engine->codec_params.rm_rate;

		if (a_engine->codec_params.adv_channels > 1) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d %s/%d/%d\n", 
							a_engine->codec_params.agreed_pt, a_engine->codec_params.rm_encoding, rate, a_engine->codec_params.adv_channels);
		} else {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d %s/%d\n", 
							a_engine->codec_params.agreed_pt, a_engine->codec_params.rm_encoding, rate);
		}

		if (fmtp_out) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=fmtp:%d %s\n", a_engine->codec_params.agreed_pt, fmtp_out);
		}

		if (a_engine->read_codec.implementation && !ptime) {
			ptime = a_engine->read_codec.implementation->microseconds_per_packet / 1000;
		}


		if ((smh->mparams->dtmf_type == DTMF_2833 || switch_media_handle_test_media_flag(smh, SCMF_LIBERAL_DTMF) || 
			 switch_channel_test_flag(session->channel, CF_LIBERAL_DTMF))
			&& smh->mparams->te > 95) {
			if (switch_channel_test_flag(session->channel, CF_WEBRTC)) {
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d telephone-event/8000\n", smh->mparams->te);
			} else {
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d telephone-event/8000\na=fmtp:%d 0-16\n", smh->mparams->te, smh->mparams->te);
			}
		}
		if (!switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) && smh->mparams->cng_pt && use_cng) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d CN/8000\n", smh->mparams->cng_pt);
			if (!a_engine->codec_params.rm_encoding) {
				smh->mparams->cng_pt = 0;
			}
		} else {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=silenceSupp:off - - - -\n");
		}

		if (append_audio) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "%s%s", append_audio, end_of(append_audio) == '\n' ? "" : "\n");
		}

		if (ptime) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ptime:%d\n", ptime);
		}


		if (local_sdp_audio_zrtp_hash) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Adding audio a=zrtp-hash:%s\n",
							  local_sdp_audio_zrtp_hash);
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=zrtp-hash:%s\n",
							local_sdp_audio_zrtp_hash);
		}

		if (!zstr(sr)) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=%s\n", sr);
		}
	

		if (!zstr(a_engine->local_dtls_fingerprint.type)) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=fingerprint:%s %s\n", a_engine->local_dtls_fingerprint.type, 
							a_engine->local_dtls_fingerprint.str);
		}
		
		if (smh->mparams->rtcp_audio_interval_msec) {
			if (a_engine->rtcp_mux > 0) {
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtcp-mux\n");
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtcp:%d IN %s %s\n", port, family, ip);
			} else {
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtcp:%d IN %s %s\n", port + 1, family, ip);
			}
		}

		//switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u\n", a_engine->ssrc);

		if (a_engine->ice_out.cands[0][0].ready) {
			char tmp1[11] = "";
			char tmp2[11] = "";
			uint32_t c1 = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - 1);
			uint32_t c2 = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - 2);
			uint32_t c3 = (2^24)*126 + (2^8)*65534 + (2^0)*(256 - 1);
			uint32_t c4 = (2^24)*126 + (2^8)*65534 + (2^0)*(256 - 2);

			tmp1[10] = '\0';
			tmp2[10] = '\0';
			switch_stun_random_string(tmp1, 10, "0123456789");
			switch_stun_random_string(tmp2, 10, "0123456789");

			ice_out = &a_engine->ice_out;
			
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u cname:%s\n", a_engine->ssrc, smh->cname);
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u msid:%s a0\n", a_engine->ssrc, smh->msid);
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u mslabel:%s\n", a_engine->ssrc, smh->msid);
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u label:%sa0\n", a_engine->ssrc, smh->msid);
			


			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ice-ufrag:%s\n", ice_out->ufrag);
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ice-pwd:%s\n", ice_out->pwd);


			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 1 %s %u %s %d typ host generation 0\n", 
							tmp1, ice_out->cands[0][0].transport, c1,
							ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port
							);

			if (!zstr(a_engine->codec_params.local_sdp_ip) && !zstr(ice_out->cands[0][0].con_addr) && 
				strcmp(a_engine->codec_params.local_sdp_ip, ice_out->cands[0][0].con_addr)
				&& a_engine->codec_params.local_sdp_port != ice_out->cands[0][0].con_port) {

				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 1 %s %u %s %d typ srflx raddr %s rport %d generation 0\n", 
								tmp2, ice_out->cands[0][0].transport, c3,
								ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port,
								a_engine->codec_params.local_sdp_ip, a_engine->codec_params.local_sdp_port
								);
			}


			if (a_engine->rtcp_mux < 1 || switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {

				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 2 %s %u %s %d typ host generation 0\n", 
								tmp1, ice_out->cands[0][0].transport, c2,
								ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port + (a_engine->rtcp_mux > 0 ? 0 : 1)
								);
				

				
				if (!zstr(a_engine->codec_params.local_sdp_ip) && !zstr(ice_out->cands[0][0].con_addr) && 
					strcmp(a_engine->codec_params.local_sdp_ip, ice_out->cands[0][0].con_addr)
					&& a_engine->codec_params.local_sdp_port != ice_out->cands[0][0].con_port) {			
					
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 2 %s %u %s %d typ srflx raddr %s rport %d generation 0\n", 
									tmp2, ice_out->cands[0][0].transport, c4,
									ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port + (a_engine->rtcp_mux > 0 ? 0 : 1),
									a_engine->codec_params.local_sdp_ip, a_engine->codec_params.local_sdp_port + (a_engine->rtcp_mux > 0 ? 0 : 1)
									);
				}
			}
			
				
#ifdef GOOGLE_ICE
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ice-options:google-ice\n");
#endif
		}




		if (!zstr(local_audio_crypto_key) && switch_channel_test_flag(session->channel, CF_SECURE)) {
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=crypto:%s\n", local_audio_crypto_key);
			//switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=encryption:optional\n");
		}

	} else if (smh->mparams->num_codecs) {
		int i;
		int cur_ptime = 0, this_ptime = 0, cng_type = 0;
		const char *mult;

		if (!switch_media_handle_test_media_flag(smh, SCMF_SUPPRESS_CNG) && smh->mparams->cng_pt && use_cng) {
			cng_type = smh->mparams->cng_pt;

			if (!a_engine->codec_params.rm_encoding) {
				smh->mparams->cng_pt = 0;
			}
		}
		
		mult = switch_channel_get_variable(session->channel, "sdp_m_per_ptime");

		
		if (switch_channel_test_flag(session->channel, CF_WEBRTC) || (mult && switch_false(mult))) {
			char *bp = buf;
			int both = switch_channel_test_flag(session->channel, CF_WEBRTC) ? 0 : 1;

			if ((!zstr(local_audio_crypto_key) && switch_channel_test_flag(session->channel, CF_SECURE)) || 
				switch_channel_test_flag(session->channel, CF_DTLS)) {
				generate_m(session, buf, SDPBUFLEN, port, family, ip, 0, append_audio, sr, use_cng, cng_type, map, 1);
				bp = (buf + strlen(buf));

				/* asterisk can't handle AVP and SAVP in sep streams, way to blow off the spec....*/
				if (switch_true(switch_channel_get_variable(session->channel, "sdp_secure_savp_only"))) {
					both = 0;
				}

			}

			if (both) {
				generate_m(session, bp, SDPBUFLEN - strlen(buf), port, family, ip, 0, append_audio, sr, use_cng, cng_type, map, 0);
			}

		} else {

			for (i = 0; i < smh->mparams->num_codecs; i++) {
				const switch_codec_implementation_t *imp = smh->codecs[i];
				
				if (imp->codec_type != SWITCH_CODEC_TYPE_AUDIO) {
					continue;
				}
				
				this_ptime = imp->microseconds_per_packet / 1000;
				
				if (!strcasecmp(imp->iananame, "ilbc") || !strcasecmp(imp->iananame, "isac")) {
					this_ptime = 20;
				}
				
				if (cur_ptime != this_ptime) {
					char *bp = buf;
					int both = 1;

					cur_ptime = this_ptime;			
					
					if ((!zstr(local_audio_crypto_key) && switch_channel_test_flag(session->channel, CF_SECURE)) || 
						switch_channel_test_flag(session->channel, CF_DTLS)) {
						generate_m(session, bp, SDPBUFLEN - strlen(buf), port, family, ip, cur_ptime, append_audio, sr, use_cng, cng_type, map, 1);
						bp = (buf + strlen(buf));

						/* asterisk can't handle AVP and SAVP in sep streams, way to blow off the spec....*/
						if (switch_true(switch_channel_get_variable(session->channel, "sdp_secure_savp_only"))) {
							both = 0;
						}
					}

					if (switch_channel_test_flag(session->channel, CF_WEBRTC)) {
						both = 0;
					}

					if (both) {
						generate_m(session, bp, SDPBUFLEN - strlen(buf), port, family, ip, cur_ptime, append_audio, sr, use_cng, cng_type, map, 0);
					}
				}
				
			}
		}

	}
	
	if (switch_channel_test_flag(session->channel, CF_VIDEO_POSSIBLE)) {
		const char *local_video_crypto_key = switch_core_session_local_crypto_key(session, SWITCH_MEDIA_TYPE_VIDEO);

		if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND) {
			if (switch_channel_test_flag(smh->session->channel, CF_DTLS)) {
				local_video_crypto_key = NULL;
			}
		}

		
		if (!v_engine->codec_params.local_sdp_port) {
			switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_VIDEO, 0);
		}

		if ((v_port = v_engine->codec_params.adv_sdp_port)) {

			if (switch_channel_test_flag(smh->session->channel, CF_ICE)) {
				gen_ice(session, SWITCH_MEDIA_TYPE_VIDEO, ip, (switch_port_t)v_port);
			}
			/*
			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "m=video %d RTP/%sAVP%s", 
							v_port, ((!zstr(local_video_crypto_key) || switch_channel_test_flag(session->channel, CF_DTLS)) 
									 && switch_channel_test_flag(session->channel, CF_SECURE)) ? "S" : "",
							switch_channel_test_flag(session->channel, CF_WEBRTC) ? "F" : "");
			*/

			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "m=video %d %s", 
							v_port, get_media_profile_name(session,
														   (!zstr(local_video_crypto_key) || switch_channel_test_flag(session->channel, CF_DTLS))
														   && switch_channel_test_flag(session->channel, CF_SECURE)));
							
			

			/*****************************/
			if (v_engine->codec_params.rm_encoding) {
				switch_core_media_set_video_codec(session, 0);
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), " %d", v_engine->codec_params.agreed_pt);
			} else if (smh->mparams->num_codecs) {
				int i;
				int already_did[128] = { 0 };
				for (i = 0; i < smh->mparams->num_codecs; i++) {
					const switch_codec_implementation_t *imp = smh->codecs[i];

					if (imp->codec_type != SWITCH_CODEC_TYPE_VIDEO) {
						continue;
					}

					if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND &&
						switch_channel_test_flag(session->channel, CF_NOVIDEO)) {
						continue;
					}

					if (smh->ianacodes[i] < 128) {
						if (already_did[smh->ianacodes[i]]) {
							continue;
						}
						already_did[smh->ianacodes[i]] = 1;
					}

					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), " %d", smh->ianacodes[i]);
					if (!ptime) {
						ptime = imp->microseconds_per_packet / 1000;
					}
				}
			}

			switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "\n");

			
			if (v_engine->codec_params.rm_encoding) {
				const char *of;
				
				if (!strcasecmp(v_engine->codec_params.rm_encoding, "VP8")) {
					vp8 = v_engine->codec_params.pt;
				}

				rate = v_engine->codec_params.rm_rate;
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d %s/%ld\n",
								v_engine->codec_params.pt, v_engine->codec_params.rm_encoding,
								v_engine->codec_params.rm_rate);

				if (switch_channel_test_flag(session->channel, CF_RECOVERING)) {
					pass_fmtp = v_engine->codec_params.rm_fmtp;
				} else {

					pass_fmtp = NULL;

					if (switch_channel_get_partner_uuid(session->channel)) {
						if ((of = switch_channel_get_variable_partner(session->channel, "rtp_video_fmtp"))) {
							pass_fmtp = of;
						}
					}

					if (ov_fmtp) {
						pass_fmtp = ov_fmtp;
					}// else { // seems to break eyebeam at least...
						//pass_fmtp = switch_channel_get_variable(session->channel, "rtp_video_fmtp");
					//}
				}

				if (pass_fmtp) {
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=fmtp:%d %s\n", v_engine->codec_params.pt, pass_fmtp);
				}

				if (append_video) {
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "%s%s", append_video, end_of(append_video) == '\n' ? "" : "\n");
				}

			} else if (smh->mparams->num_codecs) {
				int i;
				int already_did[128] = { 0 };

				for (i = 0; i < smh->mparams->num_codecs; i++) {
					const switch_codec_implementation_t *imp = smh->codecs[i];
					char *fmtp = NULL;
					uint32_t ianacode = smh->ianacodes[i];
					int channels;

					if (imp->codec_type != SWITCH_CODEC_TYPE_VIDEO) {
						continue;
					}

					if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND &&
						switch_channel_test_flag(session->channel, CF_NOVIDEO)) {
						continue;
					}

					if (ianacode < 128) {
						if (already_did[ianacode]) {
							continue;
						}
						already_did[ianacode] = 1;
					}

					if (!rate) {
						rate = imp->samples_per_second;
					}
					
					channels = get_channels(imp);

					if (!strcasecmp(imp->iananame, "VP8")) {
						vp8 = ianacode;
					}

					if (channels > 1) {
						switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d %s/%d/%d\n", ianacode, imp->iananame,
										imp->samples_per_second, channels);
					} else {
						switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtpmap:%d %s/%d\n", ianacode, imp->iananame,
										imp->samples_per_second);
					}
					
					if (!zstr(ov_fmtp)) {
						fmtp = (char *) ov_fmtp;
					} else {
					
						if (map) {
							fmtp = switch_event_get_header(map, imp->iananame);
						}
						
						if (zstr(fmtp)) fmtp = imp->fmtp;

						if (zstr(fmtp)) fmtp = (char *) pass_fmtp;
					}
					
					if (!zstr(fmtp) && strcasecmp(fmtp, "_blank_")) {
						switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=fmtp:%d %s\n", ianacode, fmtp);
					}
				}
				
			}

			if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND && switch_channel_test_flag(smh->session->channel, CF_DTLS)) {
				generate_local_fingerprint(smh, SWITCH_MEDIA_TYPE_VIDEO);
			}


			if (!zstr(v_engine->local_dtls_fingerprint.type)) {
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=fingerprint:%s %s\n", v_engine->local_dtls_fingerprint.type, 
								v_engine->local_dtls_fingerprint.str);
			}


			if (smh->mparams->rtcp_video_interval_msec) {
				if (v_engine->rtcp_mux > 0) {
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtcp-mux\n");
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtcp:%d IN %s %s\n", v_port, family, ip);
				} else {
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=rtcp:%d IN %s %s\n", v_port + 1, family, ip);
				}
			}

			//switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u\n", v_engine->ssrc);

			if (v_engine->ice_out.cands[0][0].ready) {
				char tmp1[11] = "";
				char tmp2[11] = "";
				uint32_t c1 = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - 1);
				uint32_t c2 = (2^24)*126 + (2^8)*65535 + (2^0)*(256 - 2);
				uint32_t c3 = (2^24)*126 + (2^8)*65534 + (2^0)*(256 - 1);
				uint32_t c4 = (2^24)*126 + (2^8)*65534 + (2^0)*(256 - 2);
				const char *vbw;
				int bw = 256;
				
				tmp1[10] = '\0';
				tmp2[10] = '\0';
				switch_stun_random_string(tmp1, 10, "0123456789");
				switch_stun_random_string(tmp2, 10, "0123456789");

				ice_out = &v_engine->ice_out;


				if ((vbw = switch_channel_get_variable(smh->session->channel, "rtp_video_max_bandwidth"))) {
					int v = atoi(vbw);
					bw = v;
				}
				
				if (bw > 0) {
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "b=AS:%d\n", bw);
				}


				if (vp8) {
					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), 
									"a=rtcp-fb:%d ccm fir\n", vp8);
				}
				
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u cname:%s\n", v_engine->ssrc, smh->cname);
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u msid:%s v0\n", v_engine->ssrc, smh->msid);
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u mslabel:%s\n", v_engine->ssrc, smh->msid);
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ssrc:%u label:%sv0\n", v_engine->ssrc, smh->msid);
				

				
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ice-ufrag:%s\n", ice_out->ufrag);
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ice-pwd:%s\n", ice_out->pwd);


				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 1 %s %u %s %d typ host generation 0\n", 
								tmp1, ice_out->cands[0][0].transport, c1,
								ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port
								);

				if (!zstr(v_engine->codec_params.local_sdp_ip) && !zstr(ice_out->cands[0][0].con_addr) && 
					strcmp(v_engine->codec_params.local_sdp_ip, ice_out->cands[0][0].con_addr)
					&& v_engine->codec_params.local_sdp_port != ice_out->cands[0][0].con_port) {

					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 1 %s %u %s %d typ srflx raddr %s rport %d generation 0\n", 
									tmp2, ice_out->cands[0][0].transport, c3,
									ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port,
									v_engine->codec_params.local_sdp_ip, v_engine->codec_params.local_sdp_port
									);
				}


				if (v_engine->rtcp_mux < 1 || switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_OUTBOUND) {

					switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 2 %s %u %s %d typ host generation 0\n", 
									tmp1, ice_out->cands[0][0].transport, c2,
									ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port + (v_engine->rtcp_mux > 0 ? 0 : 1)
									);
					
					
					if (!zstr(v_engine->codec_params.local_sdp_ip) && !zstr(ice_out->cands[0][1].con_addr) && 
						strcmp(v_engine->codec_params.local_sdp_ip, ice_out->cands[0][1].con_addr)
						&& v_engine->codec_params.local_sdp_port != ice_out->cands[0][1].con_port) {
						
						switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=candidate:%s 2 %s %u %s %d typ srflx generation 0\n", 
										tmp2, ice_out->cands[0][0].transport, c4,
										ice_out->cands[0][0].con_addr, ice_out->cands[0][0].con_port + (v_engine->rtcp_mux > 0 ? 0 : 1),
										v_engine->codec_params.local_sdp_ip, v_engine->codec_params.local_sdp_port + (v_engine->rtcp_mux > 0 ? 0 : 1)
										);
					}
				}

			
				
#ifdef GOOGLE_ICE
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=ice-options:google-ice\n");
#endif
			}

				


			if (switch_channel_test_flag(session->channel, CF_SECURE) && !zstr(local_video_crypto_key)){
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=crypto:%s\n", local_video_crypto_key);
				//switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "a=encryption:optional\n");
			}			

			
			if (local_sdp_video_zrtp_hash) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Adding video a=zrtp-hash:%s\n", local_sdp_video_zrtp_hash);
				switch_snprintf(buf + strlen(buf), SDPBUFLEN - strlen(buf), "a=zrtp-hash:%s\n", local_sdp_video_zrtp_hash);
			}
		}
	}


	if (map) {
		switch_event_destroy(&map);
	}
	
	if (ptmap) {
		switch_event_destroy(&ptmap);
	}

	switch_core_media_set_local_sdp(session, buf, SWITCH_TRUE);

	switch_safe_free(buf);
}

//?
SWITCH_DECLARE(void) switch_core_media_absorb_sdp(switch_core_session_t *session)
{
	const char *sdp_str;
	switch_rtp_engine_t *a_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];

	if ((sdp_str = switch_channel_get_variable(session->channel, SWITCH_B_SDP_VARIABLE))) {
		sdp_parser_t *parser;
		sdp_session_t *sdp;
		sdp_media_t *m;
		sdp_connection_t *connection;

		if ((parser = sdp_parse(NULL, sdp_str, (int) strlen(sdp_str), 0))) {
			if ((sdp = sdp_session(parser))) {
				for (m = sdp->sdp_media; m; m = m->m_next) {
					if (m->m_type != sdp_media_audio || !m->m_port) {
						continue;
					}

					connection = sdp->sdp_connection;
					if (m->m_connections) {
						connection = m->m_connections;
					}

					if (connection) {
						a_engine->codec_params.proxy_sdp_ip = switch_core_session_strdup(session, connection->c_address);
					}
					a_engine->codec_params.proxy_sdp_port = (switch_port_t) m->m_port;
					if (a_engine->codec_params.proxy_sdp_ip && a_engine->codec_params.proxy_sdp_port) {
						break;
					}
				}
			}
			sdp_parser_free(parser);
		}
		switch_core_media_set_local_sdp(session, sdp_str, SWITCH_TRUE);
	}
}


//?
SWITCH_DECLARE(void) switch_core_media_set_udptl_image_sdp(switch_core_session_t *session, switch_t38_options_t *t38_options, int insist)
{
	char buf[2048] = "";
	char max_buf[128] = "";
	char max_data[128] = "";
	const char *ip;
	uint32_t port;
	const char *family = "IP4";
	const char *username;
	const char *bit_removal_on = "a=T38FaxFillBitRemoval\n";
	const char *bit_removal_off = "";
	
	const char *mmr_on = "a=T38FaxTranscodingMMR\n";
	const char *mmr_off = "";

	const char *jbig_on = "a=T38FaxTranscodingJBIG\n";
	const char *jbig_off = "";
	const char *var;
	int broken_boolean;
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];


	switch_assert(t38_options);

	ip = t38_options->local_ip;
	port = t38_options->local_port;
	username = smh->mparams->sdp_username;

	var = switch_channel_get_variable(session->channel, "t38_broken_boolean");
	
	broken_boolean = switch_true(var);


	if (!ip) {
		if (!(ip = a_engine->codec_params.adv_sdp_ip)) {
			ip = a_engine->codec_params.proxy_sdp_ip;
		}
	}

	if (!ip) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "%s NO IP!\n", switch_channel_get_name(session->channel));
		return;
	}

	if (!port) {
		if (!(port = a_engine->codec_params.adv_sdp_port)) {
			port = a_engine->codec_params.proxy_sdp_port;
		}
	}

	if (!port) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CRIT, "%s NO PORT!\n", switch_channel_get_name(session->channel));
		return;
	}

	if (!smh->owner_id) {
		smh->owner_id = (uint32_t) switch_epoch_time_now(NULL) - port;
	}

	if (!smh->session_id) {
		smh->session_id = smh->owner_id;
	}

	smh->session_id++;

	family = strchr(ip, ':') ? "IP6" : "IP4";


	switch_snprintf(buf, sizeof(buf),
					"v=0\n"
					"o=%s %010u %010u IN %s %s\n"
					"s=%s\n" "c=IN %s %s\n" "t=0 0\n", username, smh->owner_id, smh->session_id, family, ip, username, family, ip);

	if (t38_options->T38FaxMaxBuffer) {
		switch_snprintf(max_buf, sizeof(max_buf), "a=T38FaxMaxBuffer:%d\n", t38_options->T38FaxMaxBuffer);
	};

	if (t38_options->T38FaxMaxDatagram) {
		switch_snprintf(max_data, sizeof(max_data), "a=T38FaxMaxDatagram:%d\n", t38_options->T38FaxMaxDatagram);
	};


	

	if (broken_boolean) {
		bit_removal_on = "a=T38FaxFillBitRemoval:1\n";
		bit_removal_off = "a=T38FaxFillBitRemoval:0\n";

		mmr_on = "a=T38FaxTranscodingMMR:1\n";
		mmr_off = "a=T38FaxTranscodingMMR:0\n";

		jbig_on = "a=T38FaxTranscodingJBIG:1\n";
		jbig_off = "a=T38FaxTranscodingJBIG:0\n";

	}
	

	switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf),
					"m=image %d udptl t38\n"
					"a=T38FaxVersion:%d\n"
					"a=T38MaxBitRate:%d\n"
					"%s"
					"%s"
					"%s"
					"a=T38FaxRateManagement:%s\n"
					"%s"
					"%s"
					"a=T38FaxUdpEC:%s\n",
					//"a=T38VendorInfo:%s\n",
					port,
					t38_options->T38FaxVersion,
					t38_options->T38MaxBitRate,
					t38_options->T38FaxFillBitRemoval ? bit_removal_on : bit_removal_off,
					t38_options->T38FaxTranscodingMMR ? mmr_on : mmr_off,
					t38_options->T38FaxTranscodingJBIG ? jbig_on : jbig_off,
					t38_options->T38FaxRateManagement,
					max_buf,
					max_data,
					t38_options->T38FaxUdpEC
					//t38_options->T38VendorInfo ? t38_options->T38VendorInfo : "0 0 0"
					);



	if (insist) {
		switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "m=audio 0 RTP/AVP 19\n");
	}

	switch_core_media_set_local_sdp(session, buf, SWITCH_TRUE);


	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s image media sdp:\n%s\n",
					  switch_channel_get_name(session->channel), smh->mparams->local_sdp_str);


}



//?
SWITCH_DECLARE(void) switch_core_media_patch_sdp(switch_core_session_t *session)
{
	switch_size_t len;
	char *p, *q, *pe, *qe;
	int has_video = 0, has_audio = 0, has_ip = 0;
	char port_buf[25] = "";
	char vport_buf[25] = "";
	char *new_sdp;
	int bad = 0;
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine, *v_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];

	if (zstr(smh->mparams->local_sdp_str)) {
		return;
	}

	len = strlen(smh->mparams->local_sdp_str) * 2;

	if (switch_channel_test_flag(session->channel, CF_ANSWERED) &&
		(switch_stristr("sendonly", smh->mparams->local_sdp_str) || switch_stristr("0.0.0.0", smh->mparams->local_sdp_str))) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Skip patch on hold SDP\n");
		return;
	}

	if (zstr(a_engine->codec_params.local_sdp_ip) || !a_engine->codec_params.local_sdp_port) {// || switch_channel_test_flag(session->channel, CF_PROXY_MEDIA)) {
		if (switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_AUDIO, 1) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "%s I/O Error\n",
							  switch_channel_get_name(session->channel));
			return;
		}
		a_engine->codec_params.iananame = switch_core_session_strdup(session, "PROXY");
		a_engine->codec_params.rm_rate = 8000;
		a_engine->codec_params.codec_ms = 20;
	}

	new_sdp = switch_core_session_alloc(session, len);
	switch_snprintf(port_buf, sizeof(port_buf), "%u", a_engine->codec_params.local_sdp_port);


	p = smh->mparams->local_sdp_str;
	q = new_sdp;
	pe = p + strlen(p);
	qe = q + len - 1;


	while (p && *p) {
		if (p >= pe) {
			bad = 1;
			goto end;
		}

		if (q >= qe) {
			bad = 2;
			goto end;
		}

		if (a_engine->codec_params.local_sdp_ip && !strncmp("c=IN IP", p, 7)) {
			strncpy(q, p, 7);
			p += 7;
			q += 7;
			strncpy(q, strchr(a_engine->codec_params.local_sdp_ip, ':') ? "6 " : "4 ", 2);
			p +=2;
			q +=2;			
			strncpy(q, a_engine->codec_params.local_sdp_ip, strlen(a_engine->codec_params.local_sdp_ip));
			q += strlen(a_engine->codec_params.local_sdp_ip);

			while (p && *p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == ':' || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f'))) {
				if (p >= pe) {
					bad = 3;
					goto end;
				}
				p++;
			}

			has_ip++;

		} else if (!strncmp("o=", p, 2)) {
			char *oe = strchr(p, '\n');
			switch_size_t len;

			if (oe) {
				const char *family = "IP4";
				char o_line[1024] = "";

				if (oe >= pe) {
					bad = 5;
					goto end;
				}

				len = (oe - p);
				p += len;


				family = strchr(smh->mparams->sipip, ':') ? "IP6" : "IP4";

				if (!smh->owner_id) {
					smh->owner_id = (uint32_t) switch_epoch_time_now(NULL) * 31821U + 13849U;
				}

				if (!smh->session_id) {
					smh->session_id = smh->owner_id;
				}

				smh->session_id++;


				snprintf(o_line, sizeof(o_line), "o=%s %010u %010u IN %s %s\n",
						 smh->mparams->sdp_username, smh->owner_id, smh->session_id, family, smh->mparams->sipip);

				strncpy(q, o_line, strlen(o_line));
				q += strlen(o_line) - 1;

			}

		} else if (!strncmp("s=", p, 2)) {
			char *se = strchr(p, '\n');
			switch_size_t len;

			if (se) {
				char s_line[1024] = "";

				if (se >= pe) {
					bad = 5;
					goto end;
				}

				len = (se - p);
				p += len;

				snprintf(s_line, sizeof(s_line), "s=%s\n", smh->mparams->sdp_username);

				strncpy(q, s_line, strlen(s_line));
				q += strlen(s_line) - 1;

			}

		} else if ((!strncmp("m=audio ", p, 8) && *(p + 8) != '0') || (!strncmp("m=image ", p, 8) && *(p + 8) != '0')) {
			strncpy(q, p, 8);
			p += 8;

			if (p >= pe) {
				bad = 4;
				goto end;
			}


			q += 8;

			if (q >= qe) {
				bad = 5;
				goto end;
			}


			strncpy(q, port_buf, strlen(port_buf));
			q += strlen(port_buf);

			if (q >= qe) {
				bad = 6;
				goto end;
			}

			while (p && *p && (*p >= '0' && *p <= '9')) {
				if (p >= pe) {
					bad = 7;
					goto end;
				}
				p++;
			}

			has_audio++;

		} else if (!strncmp("m=video ", p, 8) && *(p + 8) != '0') {
			if (!has_video) {
				switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_VIDEO, 1);
				v_engine->codec_params.rm_encoding = "PROXY-VID";
				v_engine->codec_params.rm_rate = 90000;
				v_engine->codec_params.codec_ms = 0;
				switch_snprintf(vport_buf, sizeof(vport_buf), "%u", v_engine->codec_params.adv_sdp_port);
				if (switch_channel_media_ready(session->channel) && !switch_rtp_ready(v_engine->rtp_session)) {
					switch_channel_set_flag(session->channel, CF_VIDEO_POSSIBLE);
					switch_channel_set_flag(session->channel, CF_REINVITE);
					switch_core_media_activate_rtp(session);
				}
			}

			strncpy(q, p, 8);
			p += 8;

			if (p >= pe) {
				bad = 8;
				goto end;
			}

			q += 8;

			if (q >= qe) {
				bad = 9;
				goto end;
			}

			strncpy(q, vport_buf, strlen(vport_buf));
			q += strlen(vport_buf);

			if (q >= qe) {
				bad = 10;
				goto end;
			}

			while (p && *p && (*p >= '0' && *p <= '9')) {

				if (p >= pe) {
					bad = 11;
					goto end;
				}

				p++;
			}

			has_video++;
		}

		while (p && *p && *p != '\n') {

			if (p >= pe) {
				bad = 12;
				goto end;
			}

			if (q >= qe) {
				bad = 13;
				goto end;
			}

			*q++ = *p++;
		}

		if (p >= pe) {
			bad = 14;
			goto end;
		}

		if (q >= qe) {
			bad = 15;
			goto end;
		}

		*q++ = *p++;

	}

 end:

	if (bad) {
		return;
	}


	if (switch_channel_down(session->channel)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s too late.\n", switch_channel_get_name(session->channel));
		return;
	}


	if (!has_ip && !has_audio) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s SDP has no audio in it.\n%s\n",
						  switch_channel_get_name(session->channel), smh->mparams->local_sdp_str);
		return;
	}


	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s Patched SDP\n---\n%s\n+++\n%s\n",
					  switch_channel_get_name(session->channel), smh->mparams->local_sdp_str, new_sdp);

	switch_core_media_set_local_sdp(session, new_sdp, SWITCH_FALSE);

}

//?
SWITCH_DECLARE(void) switch_core_media_start_udptl(switch_core_session_t *session, switch_t38_options_t *t38_options)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_channel_down(session->channel)) {
		return;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];


	if (switch_rtp_ready(a_engine->rtp_session)) {
		char *remote_host = switch_rtp_get_remote_host(a_engine->rtp_session);
		switch_port_t remote_port = switch_rtp_get_remote_port(a_engine->rtp_session);
		const char *err, *val;

		switch_channel_clear_flag(session->channel, CF_NOTIMER_DURING_BRIDGE);
		switch_rtp_udptl_mode(a_engine->rtp_session);

		if (!t38_options || !t38_options->remote_ip) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "No remote address\n");
			return;
		}

		if (remote_host && remote_port && remote_port == t38_options->remote_port && !strcmp(remote_host, t38_options->remote_ip)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Remote address:port [%s:%d] has not changed.\n",
							  t38_options->remote_ip, t38_options->remote_port);
			return;
		}

		if (switch_rtp_set_remote_address(a_engine->rtp_session, t38_options->remote_ip,
										  t38_options->remote_port, 0, SWITCH_TRUE, &err) != SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "IMAGE UDPTL REPORTS ERROR: [%s]\n", err);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "IMAGE UDPTL CHANGING DEST TO: [%s:%d]\n",
							  t38_options->remote_ip, t38_options->remote_port);
			if (!switch_media_handle_test_media_flag(smh, SCMF_DISABLE_RTP_AUTOADJ) && !switch_channel_test_flag(session->channel, CF_WEBRTC) &&
				!((val = switch_channel_get_variable(session->channel, "disable_udptl_auto_adjust")) && switch_true(val))) {
				/* Reactivate the NAT buster flag. */
				switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_AUTOADJ);
			}
		}
	}
}





//?
SWITCH_DECLARE(switch_status_t) switch_core_media_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	switch_media_handle_t *smh;
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_channel_down(session->channel)) {
		return SWITCH_STATUS_FALSE;
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	switch (msg->message_id) {

	case SWITCH_MESSAGE_INDICATE_VIDEO_REFRESH_REQ:
		{
			if (v_engine->rtp_session) {
				switch_rtp_video_refresh(v_engine->rtp_session);
			}
		}

		break;

	case SWITCH_MESSAGE_INDICATE_PROXY_MEDIA:
		{
			if (switch_rtp_ready(a_engine->rtp_session)) {
				if (msg->numeric_arg) {
					switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PROXY_MEDIA);
				} else {
					switch_rtp_clear_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PROXY_MEDIA);
				}
			}
		}
		break;

	case SWITCH_MESSAGE_INDICATE_JITTER_BUFFER:
		{
			if (switch_rtp_ready(a_engine->rtp_session)) {
				int len = 0, maxlen = 0, qlen = 0, maxqlen = 50, max_drift = 0;

				if (msg->string_arg) {
					char *p, *q;
					const char *s;

					if (!strcasecmp(msg->string_arg, "pause")) {
						switch_rtp_pause_jitter_buffer(a_engine->rtp_session, SWITCH_TRUE);
						goto end;
					} else if (!strcasecmp(msg->string_arg, "resume")) {
						switch_rtp_pause_jitter_buffer(a_engine->rtp_session, SWITCH_FALSE);
						goto end;
					} else if (!strncasecmp(msg->string_arg, "debug:", 6)) {
						s = msg->string_arg + 6;
						if (s && !strcmp(s, "off")) {
							s = NULL;
						}
						status = switch_rtp_debug_jitter_buffer(a_engine->rtp_session, s);
						goto end;
					}

					
					if ((len = atoi(msg->string_arg))) {
						qlen = len / (a_engine->read_impl.microseconds_per_packet / 1000);
						if (qlen < 1) {
							qlen = 3;
						}
					}
					
					if (qlen) {
						if ((p = strchr(msg->string_arg, ':'))) {
							p++;
							maxlen = atol(p);
							if ((q = strchr(p, ':'))) {
								q++;
								max_drift = abs(atol(q));
							}
						}
					}


					if (maxlen) {
						maxqlen = maxlen / (a_engine->read_impl.microseconds_per_packet / 1000);
					}
				}

				if (qlen) {
					if (maxqlen < qlen) {
						maxqlen = qlen * 5;
					}
					if (switch_rtp_activate_jitter_buffer(a_engine->rtp_session, qlen, maxqlen,
														  a_engine->read_impl.samples_per_packet, 
														  a_engine->read_impl.samples_per_second, max_drift) == SWITCH_STATUS_SUCCESS) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), 
										  SWITCH_LOG_DEBUG, "Setting Jitterbuffer to %dms (%d frames) (%d max frames) (%d max drift)\n", 
										  len, qlen, maxqlen, max_drift);
						switch_channel_set_flag(session->channel, CF_JITTERBUFFER);
						if (!switch_false(switch_channel_get_variable(session->channel, "rtp_jitter_buffer_plc"))) {
							switch_channel_set_flag(session->channel, CF_JITTERBUFFER_PLC);
						}
					} else {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), 
										  SWITCH_LOG_WARNING, "Error Setting Jitterbuffer to %dms (%d frames)\n", len, qlen);
					}
					
				} else {
					switch_rtp_deactivate_jitter_buffer(a_engine->rtp_session);
				}
			}
		}
		break;
	case SWITCH_MESSAGE_INDICATE_DEBUG_MEDIA:
		{
			switch_rtp_t *rtp = a_engine->rtp_session;
			const char *direction = msg->string_array_arg[0];

			if (direction && *direction == 'v') {
				direction++;
				rtp = v_engine->rtp_session;
			}

			if (switch_rtp_ready(rtp) && !zstr(direction) && !zstr(msg->string_array_arg[1])) {
				switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = {0};
				int both = !strcasecmp(direction, "both");
				int set = 0;

				if (both || !strcasecmp(direction, "read")) {
					flags[SWITCH_RTP_FLAG_DEBUG_RTP_READ]++;
					set++;
				}

				if (both || !strcasecmp(direction, "write")) {
					flags[SWITCH_RTP_FLAG_DEBUG_RTP_WRITE]++;
					set++;
				}

				if (set) {
					if (switch_true(msg->string_array_arg[1])) {
						switch_rtp_set_flags(rtp, flags);
					} else {
						switch_rtp_clear_flags(rtp, flags);
					}
				} else {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_ERROR, "Invalid Options\n");
				}
			}
		}
		goto end;
	case SWITCH_MESSAGE_INDICATE_TRANSCODING_NECESSARY:
		if (a_engine->rtp_session && switch_rtp_test_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833)) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING, "Pass 2833 mode may not work on a transcoded call.\n");
		}
		goto end;

	case SWITCH_MESSAGE_INDICATE_BRIDGE:
		{

			if (switch_rtp_ready(a_engine->rtp_session)) {
				const char *val;
				int ok = 0;
				
				if (!(val = switch_channel_get_variable(session->channel, "rtp_jitter_buffer_during_bridge")) || switch_false(val)) {
					if (switch_channel_test_flag(session->channel, CF_JITTERBUFFER) && switch_channel_test_cap_partner(session->channel, CC_FS_RTP)) {
						switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
										  "%s PAUSE Jitterbuffer\n", switch_channel_get_name(session->channel));					
						switch_rtp_pause_jitter_buffer(a_engine->rtp_session, SWITCH_TRUE);
						switch_set_flag(smh, SMF_JB_PAUSED);
					}
				}
				
				if (switch_channel_test_flag(session->channel, CF_PASS_RFC2833) && switch_channel_test_flag_partner(session->channel, CF_FS_RTP)) {
					switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833);
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
									  "%s activate passthru 2833 mode.\n", switch_channel_get_name(session->channel));
				}


				if ((val = switch_channel_get_variable(session->channel, "rtp_notimer_during_bridge"))) {
					ok = switch_true(val);
				} else {
					ok = switch_channel_test_flag(session->channel, CF_RTP_NOTIMER_DURING_BRIDGE);
				}

				if (ok && !switch_rtp_test_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_USE_TIMER)) {
					ok = 0;
				}

				if (ok) {
					switch_rtp_clear_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_USE_TIMER);
					switch_rtp_clear_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_NOBLOCK);
					switch_channel_set_flag(session->channel, CF_NOTIMER_DURING_BRIDGE);
				}

				if (ok && switch_channel_test_flag(session->channel, CF_NOTIMER_DURING_BRIDGE)) {
					/* these are not compat */
					ok = 0;
				} else {
					if ((val = switch_channel_get_variable(session->channel, "rtp_autoflush_during_bridge"))) {
						ok = switch_true(val);
					} else {
						ok = smh->media_flags[SCMF_RTP_AUTOFLUSH_DURING_BRIDGE];
					}
				}
				
				if (ok) {
					rtp_flush_read_buffer(a_engine->rtp_session, SWITCH_RTP_FLUSH_STICK);
					switch_channel_set_flag(session->channel, CF_AUTOFLUSH_DURING_BRIDGE);
				} else {
					rtp_flush_read_buffer(a_engine->rtp_session, SWITCH_RTP_FLUSH_ONCE);
				}
				
			}
		}
		goto end;
	case SWITCH_MESSAGE_INDICATE_UNBRIDGE:
		if (switch_rtp_ready(a_engine->rtp_session)) {
			
			if (switch_test_flag(smh, SMF_JB_PAUSED)) {
				switch_clear_flag(smh, SMF_JB_PAUSED);
				if (switch_channel_test_flag(session->channel, CF_JITTERBUFFER)) {
					switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
									  "%s RESUME Jitterbuffer\n", switch_channel_get_name(session->channel));					
					switch_rtp_pause_jitter_buffer(a_engine->rtp_session, SWITCH_FALSE);
				}
			}
			

			if (switch_rtp_test_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833)) {
				switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "%s deactivate passthru 2833 mode.\n",
								  switch_channel_get_name(session->channel));
				switch_rtp_clear_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PASS_RFC2833);
			}
			
			if (switch_channel_test_flag(session->channel, CF_NOTIMER_DURING_BRIDGE)) {
				if (!switch_rtp_test_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_UDPTL) && 
					!switch_rtp_test_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_PROXY_MEDIA)) {
					switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_USE_TIMER);
					switch_rtp_set_flag(a_engine->rtp_session, SWITCH_RTP_FLAG_NOBLOCK);
				}
				switch_channel_clear_flag(session->channel, CF_NOTIMER_DURING_BRIDGE);
			}

			if (switch_channel_test_flag(session->channel, CF_AUTOFLUSH_DURING_BRIDGE)) {
				rtp_flush_read_buffer(a_engine->rtp_session, SWITCH_RTP_FLUSH_UNSTICK);
				switch_channel_clear_flag(session->channel, CF_AUTOFLUSH_DURING_BRIDGE);
			} else {
				rtp_flush_read_buffer(a_engine->rtp_session, SWITCH_RTP_FLUSH_ONCE);
			}

		}
		goto end;
	case SWITCH_MESSAGE_INDICATE_AUDIO_SYNC:
		if (switch_rtp_ready(a_engine->rtp_session)) {
			rtp_flush_read_buffer(a_engine->rtp_session, SWITCH_RTP_FLUSH_ONCE);
		}
		goto end;

	case SWITCH_MESSAGE_INDICATE_NOMEDIA:
		{
			const char *uuid;
			switch_core_session_t *other_session;
			switch_channel_t *other_channel;
			const char *ip = NULL, *port = NULL;

			switch_channel_set_flag(session->channel, CF_PROXY_MODE);
			if (a_engine->codec_params.rm_encoding) {
				a_engine->codec_params.rm_encoding = NULL;
			}
			switch_core_media_set_local_sdp(session, NULL, SWITCH_FALSE);

			if ((uuid = switch_channel_get_partner_uuid(session->channel))
				&& (other_session = switch_core_session_locate(uuid))) {
				other_channel = switch_core_session_get_channel(other_session);
				ip = switch_channel_get_variable(other_channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE);
				port = switch_channel_get_variable(other_channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE);
				switch_core_session_rwunlock(other_session);
				if (ip && port) {
					switch_core_media_gen_local_sdp(session, ip, (switch_port_t)atoi(port), NULL, 1);
				}
			}


			if (!smh->mparams->local_sdp_str) {
				switch_core_media_absorb_sdp(session);
			}

		}
		break;


	default:
		break;
	}


	if (smh->mutex) switch_mutex_lock(smh->mutex);


	if (switch_channel_down(session->channel)) {
		status = SWITCH_STATUS_FALSE;
		goto end_lock;
	}

	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_MEDIA_RENEG:
		{
			switch_core_session_t *nsession;

			if (msg->string_arg) {
				switch_channel_set_variable(session->channel, "absolute_codec_string", NULL);
				if (*msg->string_arg == '=') {
					switch_channel_set_variable(session->channel, "codec_string", msg->string_arg);
				} else {
					switch_channel_set_variable_printf(session->channel, "codec_string", "=%s%s%s,%s", 
													   v_engine->codec_params.rm_encoding ? v_engine->codec_params.rm_encoding : "",
													   v_engine->codec_params.rm_encoding ? "," : "",
													   a_engine->codec_params.rm_encoding, msg->string_arg);					
				}



				a_engine->codec_params.rm_encoding = NULL;
				v_engine->codec_params.rm_encoding = NULL;
				switch_channel_clear_flag(session->channel, CF_VIDEO_POSSIBLE);
				switch_core_media_prepare_codecs(session, SWITCH_TRUE);
				switch_core_media_check_video_codecs(session);
				switch_core_media_gen_local_sdp(session, NULL, 0, NULL, 1);
			}

			switch_media_handle_set_media_flag(smh, SCMF_RENEG_ON_REINVITE);
			
			if (msg->numeric_arg && switch_core_session_get_partner(session, &nsession) == SWITCH_STATUS_SUCCESS) {
				msg->numeric_arg = 0;
				switch_core_session_receive_message(nsession, msg);
				switch_core_session_rwunlock(nsession);
			}

		}
		break;

	case SWITCH_MESSAGE_INDICATE_AUDIO_DATA:
		{
			if (switch_rtp_ready(a_engine->rtp_session)) {
				if (msg->numeric_arg) {
					if (switch_channel_test_flag(session->channel, CF_JITTERBUFFER)) {
						switch_rtp_pause_jitter_buffer(a_engine->rtp_session, SWITCH_TRUE);
						switch_set_flag(smh, SMF_JB_PAUSED);
					}

					rtp_flush_read_buffer(a_engine->rtp_session, SWITCH_RTP_FLUSH_UNSTICK);
					
				} else {
					if (switch_test_flag(smh, SMF_JB_PAUSED)) {
						switch_clear_flag(smh, SMF_JB_PAUSED);
						if (switch_channel_test_flag(session->channel, CF_JITTERBUFFER)) {
							switch_rtp_pause_jitter_buffer(a_engine->rtp_session, SWITCH_FALSE);
						}
					}
				}
			}
		}
		break;

	case SWITCH_MESSAGE_INDICATE_UDPTL_MODE:
		{
			switch_t38_options_t *t38_options = switch_channel_get_private(session->channel, "t38_options");

			if (t38_options) {
				switch_core_media_start_udptl(session, t38_options);
			}

		}

		
	default:
		break;
	}


 end_lock:

	if (smh->mutex) switch_mutex_unlock(smh->mutex);

 end:

	if (switch_channel_down(session->channel)) {
		status = SWITCH_STATUS_FALSE;
	}

	return status;

}

//?
SWITCH_DECLARE(void) switch_core_media_break(switch_core_session_t *session, switch_media_type_t type)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_break(smh->engines[type].rtp_session);
	}
}

//?
SWITCH_DECLARE(void) switch_core_media_kill_socket(switch_core_session_t *session, switch_media_type_t type)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_kill_socket(smh->engines[type].rtp_session);
	}
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_queue_rfc2833(switch_core_session_t *session, switch_media_type_t type, const switch_dtmf_t *dtmf)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		return switch_rtp_queue_rfc2833(smh->engines[type].rtp_session, dtmf);
	}

	return SWITCH_STATUS_FALSE;
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_queue_rfc2833_in(switch_core_session_t *session, switch_media_type_t type, const switch_dtmf_t *dtmf)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		return switch_rtp_queue_rfc2833_in(smh->engines[type].rtp_session, dtmf);
	}

	return SWITCH_STATUS_FALSE;
}

//?
SWITCH_DECLARE(uint8_t) switch_core_media_ready(switch_core_session_t *session, switch_media_type_t type)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return 0;
	}	

	return switch_rtp_ready(smh->engines[type].rtp_session);
}

//?
SWITCH_DECLARE(void) switch_core_media_set_rtp_flag(switch_core_session_t *session, switch_media_type_t type, switch_rtp_flag_t flag)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_set_flag(smh->engines[type].rtp_session, flag);
	}	
}

//?
SWITCH_DECLARE(void) switch_core_media_clear_rtp_flag(switch_core_session_t *session, switch_media_type_t type, switch_rtp_flag_t flag)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_clear_flag(smh->engines[type].rtp_session, flag);
	}	
}

//?
SWITCH_DECLARE(void) switch_core_media_set_recv_pt(switch_core_session_t *session, switch_media_type_t type, switch_payload_t pt)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_set_recv_pt(smh->engines[type].rtp_session, pt);
	}
}

//?
SWITCH_DECLARE(void) switch_core_media_set_telephony_event(switch_core_session_t *session, switch_media_type_t type, switch_payload_t te)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}	

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_set_telephony_event(smh->engines[type].rtp_session, te);
	}
}

//?
SWITCH_DECLARE(void) switch_core_media_set_telephony_recv_event(switch_core_session_t *session, switch_media_type_t type, switch_payload_t te)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		switch_rtp_set_telephony_recv_event(smh->engines[type].rtp_session, te);
	}
}

//?
SWITCH_DECLARE(switch_rtp_stats_t *) switch_core_media_get_stats(switch_core_session_t *session, switch_media_type_t type, switch_memory_pool_t *pool)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return NULL;
	}

	if (smh->engines[type].rtp_session) {
		return switch_rtp_get_stats(smh->engines[type].rtp_session, pool);
	}

	return NULL;
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_udptl_mode(switch_core_session_t *session, switch_media_type_t type)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		return switch_rtp_udptl_mode(smh->engines[type].rtp_session);
	}

	return SWITCH_STATUS_FALSE;
}

//?
SWITCH_DECLARE(stfu_instance_t *) switch_core_media_get_jb(switch_core_session_t *session, switch_media_type_t type)
{
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return NULL;
	}

	if (switch_rtp_ready(smh->engines[type].rtp_session)) {
		return switch_rtp_get_jitter_buffer(smh->engines[type].rtp_session);
	}

	return NULL;
}


//?
SWITCH_DECLARE(void) switch_core_media_set_sdp_codec_string(switch_core_session_t *session, const char *r_sdp)
{
	sdp_parser_t *parser;
	sdp_session_t *sdp;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}


	if ((parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {

		if ((sdp = sdp_session(parser))) {
			switch_core_media_set_r_sdp_codec_string(session, switch_core_media_get_codec_string(session), sdp);
		}

		sdp_parser_free(parser);
	}

}


static void add_audio_codec(sdp_rtpmap_t *map, int ptime, char *buf, switch_size_t buflen)
{
	int codec_ms = ptime;
	uint32_t map_bit_rate = 0;
	char ptstr[20] = "";
	char ratestr[20] = "";
	char bitstr[20] = "";
	switch_codec_fmtp_t codec_fmtp = { 0 };
						
	if (!codec_ms) {
		codec_ms = switch_default_ptime(map->rm_encoding, map->rm_pt);
	}

	map_bit_rate = switch_known_bitrate((switch_payload_t)map->rm_pt);
				
	if (!ptime && !strcasecmp(map->rm_encoding, "g723")) {
		ptime = codec_ms = 30;
	}
				
	if (zstr(map->rm_fmtp)) {
		if (!strcasecmp(map->rm_encoding, "ilbc")) {
			ptime = codec_ms = 30;
			map_bit_rate = 13330;
		} else if (!strcasecmp(map->rm_encoding, "isac")) {
			ptime = codec_ms = 30;
			map_bit_rate = 32000;
		}
	} else {
		if ((switch_core_codec_parse_fmtp(map->rm_encoding, map->rm_fmtp, map->rm_rate, &codec_fmtp)) == SWITCH_STATUS_SUCCESS) {
			if (codec_fmtp.bits_per_second) {
				map_bit_rate = codec_fmtp.bits_per_second;
			}
			if (codec_fmtp.microseconds_per_packet) {
				codec_ms = (codec_fmtp.microseconds_per_packet / 1000);
			}
		}
	}

	if (map->rm_rate) {
		switch_snprintf(ratestr, sizeof(ratestr), "@%uh", (unsigned int) map->rm_rate);
	}

	if (codec_ms) {
		switch_snprintf(ptstr, sizeof(ptstr), "@%di", codec_ms);
	}

	if (map_bit_rate) {
		switch_snprintf(bitstr, sizeof(bitstr), "@%db", map_bit_rate);
	}

	switch_snprintf(buf + strlen(buf), buflen - strlen(buf), ",%s%s%s%s", map->rm_encoding, ratestr, ptstr, bitstr);

}


SWITCH_DECLARE(void) switch_core_media_set_r_sdp_codec_string(switch_core_session_t *session, const char *codec_string, sdp_session_t *sdp)
{
	char buf[1024] = { 0 };
	sdp_media_t *m;
	sdp_attribute_t *attr;
	int ptime = 0, dptime = 0;
	sdp_connection_t *connection;
	sdp_rtpmap_t *map;
	short int match = 0;
	int i;
	int already_did[128] = { 0 };
	int num_codecs = 0;
	char *codec_order[SWITCH_MAX_CODECS];
	const switch_codec_implementation_t *codecs[SWITCH_MAX_CODECS] = { 0 };
	switch_channel_t *channel = switch_core_session_get_channel(session);
	int prefer_sdp = 0;
	const char *var;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}


	if ((var = switch_channel_get_variable(channel, "ep_codec_prefer_sdp")) && switch_true(var)) {
		prefer_sdp = 1;
	}
		
	if (!zstr(codec_string)) {
		char *tmp_codec_string;
		if ((tmp_codec_string = strdup(codec_string))) {
			num_codecs = switch_separate_string(tmp_codec_string, ',', codec_order, SWITCH_MAX_CODECS);
			num_codecs = switch_loadable_module_get_codecs_sorted(codecs, SWITCH_MAX_CODECS, codec_order, num_codecs);
			switch_safe_free(tmp_codec_string);
		}
	} else {
		num_codecs = switch_loadable_module_get_codecs(codecs, SWITCH_MAX_CODECS);
	}

	if (!channel || !num_codecs) {
		return;
	}

	for (attr = sdp->sdp_attributes; attr; attr = attr->a_next) {
		if (zstr(attr->a_name)) {
			continue;
		}
		if (!strcasecmp(attr->a_name, "ptime")) {
			dptime = atoi(attr->a_value);
			break;
		}
	}

	switch_core_media_find_zrtp_hash(session, sdp);
	switch_core_media_pass_zrtp_hash(session);

	for (m = sdp->sdp_media; m; m = m->m_next) {
		ptime = dptime;
		if (m->m_type == sdp_media_image && m->m_port) {
			switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",t38");
		} else if (m->m_type == sdp_media_audio && m->m_port) {
			for (attr = m->m_attributes; attr; attr = attr->a_next) {
				if (zstr(attr->a_name)) {
					continue;
				}
				if (!strcasecmp(attr->a_name, "ptime") && attr->a_value) {
					ptime = atoi(attr->a_value);
					break;
				}
			}
			connection = sdp->sdp_connection;
			if (m->m_connections) {
				connection = m->m_connections;
			}

			if (!connection) {
				switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_ERROR, "Cannot find a c= line in the sdp at media or session level!\n");
				break;
			}

			if (switch_channel_direction(channel) == SWITCH_CALL_DIRECTION_INBOUND || prefer_sdp) {
				for (map = m->m_rtpmaps; map; map = map->rm_next) {
					if (map->rm_pt > 127 || already_did[map->rm_pt]) {
						continue;
					}

					for (i = 0; i < num_codecs; i++) {
						const switch_codec_implementation_t *imp = codecs[i];

						if ((zstr(map->rm_encoding) || (smh->mparams->ndlb & SM_NDLB_ALLOW_BAD_IANANAME)) && map->rm_pt < 96) {
							match = (map->rm_pt == imp->ianacode) ? 1 : 0;
						} else {
							if (map->rm_encoding) {
								match = strcasecmp(map->rm_encoding, imp->iananame) ? 0 : 1;
							} else {
								match = 0;
							}
						}

						if (match) {
							add_audio_codec(map, ptime, buf, sizeof(buf));
							break;
						}
					
					}
				}

			} else {
				for (i = 0; i < num_codecs; i++) {
					const switch_codec_implementation_t *imp = codecs[i];
					if (imp->codec_type != SWITCH_CODEC_TYPE_AUDIO || imp->ianacode > 127 || already_did[imp->ianacode]) {
						continue;
					}
					for (map = m->m_rtpmaps; map; map = map->rm_next) {
						if (map->rm_pt > 127 || already_did[map->rm_pt]) {
							continue;
						}

						if ((zstr(map->rm_encoding) || (smh->mparams->ndlb & SM_NDLB_ALLOW_BAD_IANANAME)) && map->rm_pt < 96) {
							match = (map->rm_pt == imp->ianacode) ? 1 : 0;
						} else {
							if (map->rm_encoding) {
								match = strcasecmp(map->rm_encoding, imp->iananame) ? 0 : 1;
							} else {
								match = 0;
							}
						}

						if (match) {
							add_audio_codec(map, ptime, buf, sizeof(buf));
							break;
						}
					}
				}
			}

		} else if (m->m_type == sdp_media_video && m->m_port) {
			connection = sdp->sdp_connection;
			if (m->m_connections) {
				connection = m->m_connections;
			}

			if (!connection) {
				switch_log_printf(SWITCH_CHANNEL_CHANNEL_LOG(channel), SWITCH_LOG_ERROR, "Cannot find a c= line in the sdp at media or session level!\n");
				break;
			}
			for (i = 0; i < num_codecs; i++) {
				const switch_codec_implementation_t *imp = codecs[i];

				if (imp->codec_type != SWITCH_CODEC_TYPE_VIDEO || imp->ianacode > 127 || already_did[imp->ianacode]) {
					continue;
				}

				if (switch_channel_direction(session->channel) == SWITCH_CALL_DIRECTION_INBOUND &&
					switch_channel_test_flag(session->channel, CF_NOVIDEO)) {
					continue;
				}

				for (map = m->m_rtpmaps; map; map = map->rm_next) {
					if (map->rm_pt > 127 || already_did[map->rm_pt]) {
						continue;
					}

					if ((zstr(map->rm_encoding) || (smh->mparams->ndlb & SM_NDLB_ALLOW_BAD_IANANAME)) && map->rm_pt < 96) {
						match = (map->rm_pt == imp->ianacode) ? 1 : 0;
					} else {
						if (map->rm_encoding) {
							match = strcasecmp(map->rm_encoding, imp->iananame) ? 0 : 1;
						} else {
							match = 0;
						}
					}

					if (match) {
						if (ptime > 0) {
							switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",%s@%uh@%di", imp->iananame, (unsigned int) map->rm_rate,
											ptime);
						} else {
							switch_snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ",%s@%uh", imp->iananame, (unsigned int) map->rm_rate);
						}
						already_did[imp->ianacode] = 1;
						break;
					}
				}
			}
		}
	}
	if (buf[0] == ',') {
		switch_channel_set_variable(channel, "ep_codec_string", buf + 1);
	}
}

//?
SWITCH_DECLARE(switch_status_t) switch_core_media_codec_chosen(switch_core_session_t *session, switch_media_type_t type)
{
	switch_rtp_engine_t *engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return SWITCH_STATUS_FALSE;
	}
	
	engine = &smh->engines[type];

	if (engine->codec_params.iananame) {
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}


//?
SWITCH_DECLARE(void) switch_core_media_check_outgoing_proxy(switch_core_session_t *session, switch_core_session_t *o_session)
{
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;
	const char *r_sdp = NULL;

	switch_assert(session);

	if (!switch_channel_test_flag(o_session->channel, CF_PROXY_MEDIA)) {
		return;
	}

	if (!(smh = session->media_handle)) {
		return;
	}

	r_sdp = switch_channel_get_variable(o_session->channel, SWITCH_R_SDP_VARIABLE);
	
	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];

	switch_channel_set_flag(session->channel, CF_PROXY_MEDIA);
	
	a_engine->codec_params.iananame = switch_core_session_strdup(session, "PROXY");
	a_engine->codec_params.rm_rate = 8000;

	a_engine->codec_params.codec_ms = 20;
	
	if (switch_stristr("m=video", r_sdp)) {
		switch_core_media_choose_port(session, SWITCH_MEDIA_TYPE_VIDEO, 1);
		v_engine->codec_params.rm_encoding = "PROXY-VID";
		v_engine->codec_params.rm_rate = 90000;
		v_engine->codec_params.codec_ms = 0;
		switch_channel_set_flag(session->channel, CF_VIDEO);
		switch_channel_set_flag(session->channel, CF_VIDEO_POSSIBLE);
	}
}

#ifdef _MSC_VER
/* remove this if the break is removed from the following for loop which causes unreachable code loop */
/* for (m = sdp->sdp_media; m; m = m->m_next) { */
#pragma warning(push)
#pragma warning(disable:4702)
#endif

//?
SWITCH_DECLARE(void) switch_core_media_proxy_codec(switch_core_session_t *session, const char *r_sdp)
{
	sdp_media_t *m;
	sdp_parser_t *parser = NULL;
	sdp_session_t *sdp;
	sdp_attribute_t *attr;
	int ptime = 0, dptime = 0;
	switch_rtp_engine_t *a_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}
	
	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];

	if (!(parser = sdp_parse(NULL, r_sdp, (int) strlen(r_sdp), 0))) {
		return;
	}

	if (!(sdp = sdp_session(parser))) {
		sdp_parser_free(parser);
		return;
	}


	for (attr = sdp->sdp_attributes; attr; attr = attr->a_next) {
		if (zstr(attr->a_name)) {
			continue;
		}

		if (!strcasecmp(attr->a_name, "ptime")) {
			dptime = atoi(attr->a_value);
		}
	}


	for (m = sdp->sdp_media; m; m = m->m_next) {

		ptime = dptime;
		//maxptime = dmaxptime;

		if (m->m_proto == sdp_proto_rtp) {
			sdp_rtpmap_t *map;
			for (attr = m->m_attributes; attr; attr = attr->a_next) {
				if (!strcasecmp(attr->a_name, "ptime") && attr->a_value) {
					ptime = atoi(attr->a_value);
				} else if (!strcasecmp(attr->a_name, "maxptime") && attr->a_value) {
					//maxptime = atoi(attr->a_value);		
				}
			}

			for (map = m->m_rtpmaps; map; map = map->rm_next) {
				a_engine->codec_params.iananame = switch_core_session_strdup(session, map->rm_encoding);
				a_engine->codec_params.rm_rate = map->rm_rate;
				a_engine->codec_params.codec_ms = ptime;
				switch_core_media_set_codec(session, 0, smh->mparams->codec_flags);
				break;
			}

			break;
		}
	}

	sdp_parser_free(parser);

}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

SWITCH_DECLARE (void) switch_core_media_recover_session(switch_core_session_t *session)
{
	const char *ip;
	const char *port;
	const char *a_ip;
	const char *r_ip;
	const char *r_port;
	const char *tmp;	
	switch_rtp_engine_t *a_engine, *v_engine;
	switch_media_handle_t *smh;

	switch_assert(session);

	if (!(smh = session->media_handle)) {
		return;
	}
	
	ip = switch_channel_get_variable(session->channel, SWITCH_LOCAL_MEDIA_IP_VARIABLE);
	port = switch_channel_get_variable(session->channel, SWITCH_LOCAL_MEDIA_PORT_VARIABLE);



	if (switch_channel_test_flag(session->channel, CF_PROXY_MODE)  || !(ip && port)) {
		return;
	} else {
		a_ip = switch_channel_get_variable(session->channel, SWITCH_ADVERTISED_MEDIA_IP_VARIABLE);
		r_ip = switch_channel_get_variable(session->channel, SWITCH_REMOTE_MEDIA_IP_VARIABLE);
		r_port = switch_channel_get_variable(session->channel, SWITCH_REMOTE_MEDIA_PORT_VARIABLE);
	}

	a_engine = &smh->engines[SWITCH_MEDIA_TYPE_AUDIO];
	v_engine = &smh->engines[SWITCH_MEDIA_TYPE_VIDEO];


	a_engine->codec_params.iananame = a_engine->codec_params.rm_encoding = (char *) switch_channel_get_variable(session->channel, "rtp_use_codec_name");
	a_engine->codec_params.rm_fmtp = (char *) switch_channel_get_variable(session->channel, "rtp_use_codec_fmtp");

	if ((tmp = switch_channel_get_variable(session->channel, SWITCH_R_SDP_VARIABLE))) {
		smh->mparams->remote_sdp_str = switch_core_session_strdup(session, tmp);
	}


	if ((tmp = switch_channel_get_variable(session->channel, "rtp_last_audio_codec_string"))) {
		const char *vtmp = switch_channel_get_variable(session->channel, "rtp_last_video_codec_string");
		switch_channel_set_variable_printf(session->channel, "rtp_use_codec_string", "%s%s%s", tmp, vtmp ? "," : "", vtmp ? vtmp : "");
	}
	
	if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_codec_string"))) {
		char *tmp_codec_string = switch_core_session_strdup(smh->session, tmp);
		smh->codec_order_last = switch_separate_string(tmp_codec_string, ',', smh->codec_order, SWITCH_MAX_CODECS);
		smh->mparams->num_codecs = switch_loadable_module_get_codecs_sorted(smh->codecs, SWITCH_MAX_CODECS, smh->codec_order, smh->codec_order_last);
	}

	if ((tmp = switch_channel_get_variable(session->channel, "rtp_2833_send_payload"))) {
		smh->mparams->te = (switch_payload_t)atoi(tmp);
	}

	if ((tmp = switch_channel_get_variable(session->channel, "rtp_2833_recv_payload"))) {
		smh->mparams->recv_te = (switch_payload_t)atoi(tmp);
	}

	if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_codec_rate"))) {
		a_engine->codec_params.rm_rate = atoi(tmp);
	}

	if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_codec_ptime"))) {
		a_engine->codec_params.codec_ms = atoi(tmp);
	}

	if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_pt"))) {
		a_engine->codec_params.pt = a_engine->codec_params.agreed_pt = (switch_payload_t)atoi(tmp);
	}

	if ((tmp = switch_channel_get_variable(session->channel, "rtp_audio_recv_pt"))) {
		a_engine->codec_params.recv_pt = a_engine->codec_params.agreed_pt = (switch_payload_t)atoi(tmp);;
	}
			
	switch_core_media_set_codec(session, 1, smh->mparams->codec_flags);

	a_engine->codec_params.adv_sdp_ip = smh->mparams->extrtpip = (char *) ip;
	a_engine->codec_params.adv_sdp_port = a_engine->codec_params.local_sdp_port = (switch_port_t)atoi(port);

	if (!zstr(ip)) {
		a_engine->codec_params.local_sdp_ip = switch_core_session_strdup(session, ip);
		smh->mparams->rtpip = a_engine->codec_params.local_sdp_ip;
	}

	if (!zstr(a_ip)) {
		a_engine->codec_params.adv_sdp_ip = switch_core_session_strdup(session, a_ip);
	}

	if (r_ip && r_port) {
		a_engine->codec_params.remote_sdp_ip = (char *) r_ip;
		a_engine->codec_params.remote_sdp_port = (switch_port_t)atoi(r_port);
	}

	if (switch_channel_test_flag(session->channel, CF_VIDEO)) {
		if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_video_pt"))) {
			v_engine->codec_params.pt = v_engine->codec_params.agreed_pt = (switch_payload_t)atoi(tmp);
		}
		
		if ((tmp = switch_channel_get_variable(session->channel, "rtp_video_recv_pt"))) {
			v_engine->codec_params.recv_pt = a_engine->codec_params.agreed_pt = (switch_payload_t)atoi(tmp);;
		}

		v_engine->codec_params.rm_encoding = (char *) switch_channel_get_variable(session->channel, "rtp_use_video_codec_name");
		v_engine->codec_params.rm_fmtp = (char *) switch_channel_get_variable(session->channel, "rtp_use_video_codec_fmtp");

		ip = switch_channel_get_variable(session->channel, SWITCH_LOCAL_VIDEO_IP_VARIABLE);
		port = switch_channel_get_variable(session->channel, SWITCH_LOCAL_VIDEO_PORT_VARIABLE);
		r_ip = switch_channel_get_variable(session->channel, SWITCH_REMOTE_VIDEO_IP_VARIABLE);
		r_port = switch_channel_get_variable(session->channel, SWITCH_REMOTE_VIDEO_PORT_VARIABLE);

		switch_channel_set_flag(session->channel, CF_VIDEO_POSSIBLE);

		if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_video_codec_rate"))) {
			v_engine->codec_params.rm_rate = atoi(tmp);
		}

		if ((tmp = switch_channel_get_variable(session->channel, "rtp_use_video_codec_ptime"))) {
			v_engine->codec_params.codec_ms = atoi(tmp);
		}

		v_engine->codec_params.adv_sdp_port = v_engine->codec_params.local_sdp_port = (switch_port_t)atoi(port);

		if (r_ip && r_port) {
			v_engine->codec_params.remote_sdp_ip = (char *) r_ip;
			v_engine->codec_params.remote_sdp_port = (switch_port_t)atoi(r_port);
		}
	}

	switch_core_media_gen_local_sdp(session, NULL, 0, NULL, 1);

	if (switch_core_media_activate_rtp(session) != SWITCH_STATUS_SUCCESS) {
		return;
	}

	switch_core_session_get_recovery_crypto_key(session, SWITCH_MEDIA_TYPE_AUDIO);
	switch_core_session_get_recovery_crypto_key(session, SWITCH_MEDIA_TYPE_VIDEO);


	if ((tmp = switch_channel_get_variable(session->channel, "rtp_last_audio_local_crypto_key"))) {
		int idx = atoi(tmp);

		a_engine->ssec.local_crypto_key = switch_core_session_strdup(session, tmp);
		switch_core_media_add_crypto(&a_engine->ssec, a_engine->ssec.local_crypto_key, SWITCH_RTP_CRYPTO_SEND);
		switch_core_media_add_crypto(&a_engine->ssec, a_engine->ssec.remote_crypto_key, SWITCH_RTP_CRYPTO_RECV);
		switch_channel_set_flag(smh->session->channel, CF_SECURE);
		
		switch_rtp_add_crypto_key(a_engine->rtp_session, SWITCH_RTP_CRYPTO_SEND, idx,
								  a_engine->ssec.crypto_send_type, a_engine->ssec.local_raw_key, SWITCH_RTP_KEY_LEN);
		
		switch_rtp_add_crypto_key(a_engine->rtp_session, SWITCH_RTP_CRYPTO_RECV, a_engine->ssec.crypto_tag,
								  a_engine->ssec.crypto_recv_type, a_engine->ssec.remote_raw_key, SWITCH_RTP_KEY_LEN);
	}


	if (switch_core_media_ready(session, SWITCH_MEDIA_TYPE_AUDIO)) {
		switch_core_media_set_recv_pt(session, SWITCH_MEDIA_TYPE_AUDIO, a_engine->codec_params.recv_pt);
		switch_rtp_set_telephony_event(a_engine->rtp_session, smh->mparams->te);
		switch_rtp_set_telephony_recv_event(a_engine->rtp_session, smh->mparams->recv_te);
	}

	if (switch_core_media_ready(session, SWITCH_MEDIA_TYPE_VIDEO)) {
		switch_core_media_set_recv_pt(session, SWITCH_MEDIA_TYPE_VIDEO, v_engine->codec_params.recv_pt);
	}

}


SWITCH_DECLARE(void) switch_core_media_init(void)
{
	switch_core_gen_certs(DTLS_SRTP_FNAME);	
}

SWITCH_DECLARE(void) switch_core_media_deinit(void)
{
	
}


/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */

