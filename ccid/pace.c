/*
 * Copyright (C) 2010 Frank Morgner
 *
 * This file is part of ccid.
 *
 * ccid is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * ccid is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * ccid.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <opensc/log.h>
#include "pace.h"

#ifdef NO_PACE
inline int GetReadersPACECapabilities(sc_context_t *ctx, sc_card_t *card,
        const __u8 *in, __u8 **out, size_t *outlen) {
    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_ERROR_NOT_SUPPORTED);
}
inline int EstablishPACEChannel(sc_context_t *ctx, sc_card_t *card,
        const __u8 *in, __u8 **out, size_t *outlen) {
    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_ERROR_NOT_SUPPORTED);
}
#else

#include <opensc/opensc.h>
#include <opensc/ui.h>
#include <openssl/pace.h>
#include <openssl/objects.h>
#include <openssl/err.h>
#include <asm/byteorder.h>
#include <string.h>

uint16_t ssc = 0;

int GetReadersPACECapabilities(sc_context_t *ctx, sc_card_t *card,
        const __u8 *in, __u8 **out, size_t *outlen) {
    if (!out || !outlen)
        SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_ERROR_INVALID_ARGUMENTS);

    __u8 *result = realloc(*out, 2);
    if (!result)
        SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_ERROR_OUT_OF_MEMORY);
    *out = result;
    *outlen = 2;

    /* lengthBitMap */
    *result = 1;
    result++;
    /* BitMap */
    *result = PACE_BITMAP_PACE|PACE_BITMAP_EID;

    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_SUCCESS);
}

/** select and read EF.CardAccess */
int get_ef_card_access(sc_context_t *ctx, sc_card_t *card,
        __u8 **ef_cardaccess, size_t *length_ef_cardaccess)
{
    int r;
    char buf[9];
    sc_path_t path;
    sc_file_t *file;

    snprintf(buf, sizeof buf, "3f00%04x", FID_EF_CARDACCESS);
    sc_format_path(buf, &path);

    SC_TEST_RET(ctx, sc_select_file(card, &path, &file),
            "Could not select EF.CardAccess.");
    ssc++;

    *length_ef_cardaccess = file->size;
    *ef_cardaccess = malloc(*length_ef_cardaccess);
    if (!*ef_cardaccess)
        SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_ERROR_OUT_OF_MEMORY);

    r = sc_read_binary(card, 0, *ef_cardaccess, *length_ef_cardaccess, 0);
    ssc++;
    SC_TEST_RET(ctx, r, "Could not read EF.CardAccess.");
    *length_ef_cardaccess = r;

    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_SUCCESS);
}

int pace_mse_set_at(sc_context_t *ctx, sc_card_t *card,
        int protocol, int secret_key, int reference)
{
    sc_apdu_t apdu;
    unsigned char *d = NULL;
    PACE_MSE_SET_AT_C *data = NULL;
    int r;

    memset(&apdu, 0, sizeof apdu);
    apdu.ins = 0x22;
    apdu.p1 = 0xc1;
    apdu.p2 = 0xa4;
    apdu.flags = SC_APDU_FLAGS_NO_GET_RESP|SC_APDU_FLAGS_NO_RETRY_WL;

    data = PACE_MSE_SET_AT_C_new();
    if (!data) {
        r = SC_ERROR_OUT_OF_MEMORY;
        goto err;
    }
    data->cryptographic_mechanism_reference = OBJ_nid2obj(protocol);
    data->key_reference1 = ASN1_INTEGER_new();
    data->key_reference2 = ASN1_INTEGER_new();
    if (!data->cryptographic_mechanism_reference
            || !data->key_reference1
            || !data->key_reference2) {
        r = SC_ERROR_OUT_OF_MEMORY;
        goto err;
    }
    if (!ASN1_INTEGER_set(data->key_reference1, secret_key)
            || !ASN1_INTEGER_set(data->key_reference2, reference)) {
        r = SC_ERROR_INTERNAL;
        goto err;
    }
    r = i2d_PACE_MSE_SET_AT_C(data, &d);
    if (r < 0) {
        r = SC_ERROR_INTERNAL;
        goto err;
    }
    apdu.data = (const u8 *) d;
    apdu.datalen = r;
    apdu.lc = r;

    r = sc_transmit_apdu(card, &apdu);
    ssc++;
    if (r < 0)
        goto err;

    if (apdu.resplen) {
        r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
        goto err;
    }

    if (apdu.sw1 == 0x63) {
        if ((apdu.sw2 & 0xc0) == 0xc0) {
             sc_error(card->ctx, "Verification failed (remaining tries: %d%s)\n",
                   apdu.sw2 & 0x0f,
                   (apdu.sw2 & 0x0f) == 1? ", password must be resumed":
                   (apdu.sw2 & 0x0f) == 0? ", password must be unblocked":
                   "");
             r = SC_ERROR_PIN_CODE_INCORRECT;
             goto err;
        } else {
            sc_error(card->ctx, "Unknown SWs; SW1=%02X, SW2=%02X\n",
                    apdu.sw1, apdu.sw2);
            r = SC_ERROR_CARD_CMD_FAILED;
            goto err;
        }
    } else if (apdu.sw1 == 0x62 && apdu.sw2 == 0x83) {
             sc_error(card->ctx, "Password is deactivated\n");
             r = SC_ERROR_AUTH_METHOD_BLOCKED;
             goto err;
    } else {
        r = sc_check_sw(card, apdu.sw1, apdu.sw2);
    }

err:
    if (data) {
        if (data->cryptographic_mechanism_reference)
            ASN1_OBJECT_free(data->cryptographic_mechanism_reference);
        if (data->key_reference1)
            ASN1_INTEGER_free(data->key_reference1);
        if (data->key_reference2)
            ASN1_INTEGER_free(data->key_reference2);
        PACE_MSE_SET_AT_C_free(data);
    }
    if (d)
        free(d);
    if (apdu.resplen)
        free(apdu.resp);

    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, r);
}

int pace_gen_auth(sc_context_t *ctx, sc_card_t *card,
        int step, const u8 *in, size_t in_len, u8 **out, size_t *out_len)
{
    sc_apdu_t apdu;
    PACE_GEN_AUTH_C *c_data = NULL;
    PACE_GEN_AUTH_R *r_data = NULL;
    unsigned char *d = NULL, *p;
    int r, l;

    memset(&apdu, 0, sizeof apdu);
    apdu.ins = 0x86;
    apdu.p1 = 0;
    apdu.p2 = 0;
    apdu.flags = SC_APDU_FLAGS_NO_GET_RESP|SC_APDU_FLAGS_NO_RETRY_WL;

    c_data = PACE_GEN_AUTH_C_new();
    if (!c_data) {
        r = SC_ERROR_OUT_OF_MEMORY;
        goto err;
    }
    switch(step) {
        case 1:
            break;
        case 2:
            c_data->mapping_data = ASN1_OCTET_STRING_new();
            if (!c_data->mapping_data
                    || !M_ASN1_OCTET_STRING_set(
                        c_data->mapping_data, in, in_len)) {
                r = SC_ERROR_INTERNAL;
                goto err;
            }
            break;
        case 3:
            c_data->eph_pub_key = ASN1_OCTET_STRING_new();
            if (!c_data->eph_pub_key
                    || !M_ASN1_OCTET_STRING_set(
                        c_data->eph_pub_key, in, in_len)) {
                r = SC_ERROR_INTERNAL;
                goto err;
            }
            break;
        case 4:
            c_data->auth_token = ASN1_OCTET_STRING_new();
            if (!c_data->auth_token
                    || !M_ASN1_OCTET_STRING_set(
                        c_data->auth_token, in, in_len)) {
                r = SC_ERROR_INTERNAL;
                goto err;
            }
            break;
        default:
            r = SC_ERROR_INVALID_ARGUMENTS;
            goto err;
    }
    r = i2d_PACE_GEN_AUTH_C(c_data, &d);
    if (r < 0) {
        r = SC_ERROR_INTERNAL;
        goto err;
    }
    apdu.data = (const u8 *) d;
    apdu.datalen = r;
    apdu.lc = r;

    r = sc_transmit_apdu(card, &apdu);
    ssc++;
    if (r < 0)
        goto err;

    r = sc_check_sw(card, apdu.sw1, apdu.sw2);
    if (r < 0)
        goto err;

    if (!d2i_PACE_GEN_AUTH_R(&r_data,
                (const unsigned char **) &apdu.resp, apdu.resplen)) {
        r = SC_ERROR_INTERNAL;
        goto err;
    }

    switch(step) {
        case 1:
            if (!r_data->enc_nonce
                    || r_data->mapping_data
                    || r_data->eph_pub_key
                    || r_data->auth_token) {
                r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
                goto err;
            }
            p = r_data->enc_nonce->data;
            l = r_data->enc_nonce->length;
            break;
        case 2:
            if (r_data->enc_nonce
                    || !r_data->mapping_data
                    || r_data->eph_pub_key
                    || r_data->auth_token) {
                r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
                goto err;
            }
            p = r_data->mapping_data->data;
            l = r_data->mapping_data->length;
            break;
        case 3:
            if (r_data->enc_nonce
                    || r_data->mapping_data
                    || !r_data->eph_pub_key
                    || r_data->auth_token) {
                r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
                goto err;
            }
            p = r_data->eph_pub_key->data;
            l = r_data->eph_pub_key->length;
            break;
        case 4:
            if (r_data->enc_nonce
                    || r_data->mapping_data
                    || r_data->eph_pub_key
                    || !r_data->auth_token) {
                r = SC_ERROR_UNKNOWN_DATA_RECEIVED;
                goto err;
            }
            p = r_data->auth_token->data;
            l = r_data->auth_token->length;
            break;
        default:
            r = SC_ERROR_INVALID_ARGUMENTS;
            goto err;
    }

    *out = malloc(l);
    if (!*out) {
        r = SC_ERROR_OUT_OF_MEMORY;
        goto err;
    }
    memcpy(p, *out, l);
    *out_len = l;

err:
    if (c_data) {
        if (c_data->mapping_data)
            ASN1_OCTET_STRING_free(c_data->mapping_data);
        if (c_data->eph_pub_key)
            ASN1_OCTET_STRING_free(c_data->eph_pub_key);
        if (c_data->auth_token)
            ASN1_OCTET_STRING_free(c_data->auth_token);
        PACE_GEN_AUTH_C_free(c_data);
    }
    if (d)
        free(d);
    if (r_data) {
        if (r_data->mapping_data)
            ASN1_OCTET_STRING_free(r_data->mapping_data);
        if (r_data->eph_pub_key)
            ASN1_OCTET_STRING_free(r_data->eph_pub_key);
        if (r_data->auth_token)
            ASN1_OCTET_STRING_free(r_data->auth_token);
        PACE_GEN_AUTH_R_free(r_data);
    }
    if (apdu.resplen)
        free(apdu.resp);

    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, r);
}

PACE_SEC *
get_psec(sc_card_t *card, const char *pin, size_t length_pin, u8 pin_id)
{
    sc_ui_hints_t hints;
    char *p;
    PACE_SEC *r;
    int sc_result;

    if (pin && length_pin)
       return PACE_SEC_new(pin, length_pin, pin_id);

    memset(&hints, 0, sizeof(hints));
    hints.dialog_name = "ccid.PACE";
    hints.card = card;
    switch(pin_id) {
        case PACE_MRZ:
            hints.prompt = "Enter MRZ";
            break;
        case PACE_CAN:
            hints.prompt = "Enter CAN";
            break;
        case PACE_PIN:
            hints.prompt = "Enter PIN";
            break;
        case PACE_PUK:
            hints.prompt = "Enter PUK";
            break;
        default:
            return NULL;
    }
    hints.usage = SC_UI_USAGE_OTHER;
    sc_result = sc_ui_get_pin(&hints, &p);
    if (sc_result < 0) {
        sc_error(card->ctx, "Could not read PACE secret (%s).\n",
                sc_strerror(sc_result));
        return NULL;
    }

    r = PACE_SEC_new(p, strlen(p), pin_id);

    OPENSSL_cleanse(p, strlen(p));
    free(p);

    return r;
}

void debug_ossl(sc_context_t *ctx) {
    unsigned long r = ERR_get_error();
    while (r) {
        sc_error(ctx, ERR_error_string(r, NULL));
    }
}

int EstablishPACEChannel(sc_context_t *ctx, sc_card_t *card,
        const __u8 *in, __u8 **out, size_t *outlen)
{
    __u8 pin_id;
    size_t length_chat, length_pin, length_cert_desc, length_ef_cardaccess;
    const __u8 *chat, *pin, *certificate_description;
    __u8 *ef_cardaccess;
    PACEInfo *info = NULL;
    PACEDomainParameterInfo *static_dp = NULL, *eph_dp = NULL;
    BUF_MEM *enc_nonce, *nonce = NULL, *mdata = NULL, *mdata_opp = NULL,
            *k_enc = NULL, *k_mac = NULL, *token_opp = NULL,
            *token = NULL, *pub = NULL, *pub_opp = NULL, *key = NULL;
    PACE_SEC *sec = NULL;
    PACE_CTX *pctx = NULL;
    int r;

    if (!in || !out || !outlen)
        SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, SC_ERROR_INVALID_ARGUMENTS);

    pin_id = *in;
    in++;

    length_chat = *in;
    in++;
    chat = in;
    in += length_chat;

    length_pin = *in;
    in++;
    pin = in;
    in += length_pin;

    length_cert_desc = (__le16_to_cpu((__le16) *in));
    in += sizeof (__le16);
    certificate_description = in;

    enc_nonce = BUF_MEM_new();
    if (!enc_nonce) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    r = get_ef_card_access(ctx, card, &ef_cardaccess, &length_ef_cardaccess);
    if (r < 0) {
        goto err;
    }
    if (!parse_ef_card_access(ef_cardaccess, length_ef_cardaccess,
                &info, &static_dp)) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    r = pace_mse_set_at(ctx, card, info->protocol, pin_id, 1);
    if (r < 0) {
        goto err;
    }
    r = pace_gen_auth(ctx, card, 1, NULL, 0, (u8 **) &enc_nonce->data,
            &enc_nonce->length);
    if (r < 0) {
        goto err;
    }
    enc_nonce->max = enc_nonce->length;

    sec = get_psec(card, (char *) pin, length_pin, pin_id);
    if (!sec) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    pctx = PACE_CTX_new();
    if (!pctx || !PACE_CTX_init(pctx, info)) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }

    nonce = PACE_STEP2_dec_nonce(sec, enc_nonce, pctx);

    mdata_opp = BUF_MEM_new();
    mdata = PACE_STEP3A_map_generate_key(static_dp, pctx);
    if (!nonce || !mdata || !mdata_opp) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    r = pace_gen_auth(ctx, card, 2, (u8 *) mdata->data, mdata->length,
            (u8 **) &mdata_opp->data, &mdata_opp->length);
    if (r < 0) {
        goto err;
    }
    mdata_opp->max = mdata_opp->length;

    eph_dp = PACE_STEP3A_map_compute_key(static_dp, pctx, nonce, mdata_opp);
    pub = PACE_STEP3B_dh_generate_key(eph_dp, pctx);
    pub_opp = BUF_MEM_new();
    if (!eph_dp || !pub || !pub_opp) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    r = pace_gen_auth(ctx, card, 3, (u8 *) pub->data, pub->length,
            (u8 **) &pub_opp->data, &pub_opp->length);
    if (r < 0) {
        goto err;
    }
    pub_opp->max = pub_opp->length;

    key = PACE_STEP3B_dh_compute_key(eph_dp, pctx, pub_opp);
    if (!key ||
            !PACE_STEP3C_derive_keys(key, pctx, &k_mac, &k_enc)) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    token = PACE_STEP3D_compute_authentication_token(pctx,
            eph_dp, info, pub_opp, k_mac, ssc);
    token_opp = BUF_MEM_new();
    if (!token || !token_opp) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }
    r = pace_gen_auth(ctx, card, 4, (u8 *) token->data, token->length,
            (u8 **) &token_opp->data, &token_opp->length);
    if (r < 0) {
        goto err;
    }
    token_opp->max = token_opp->length;

    if (!PACE_STEP3D_verify_authentication_token(pctx,
            eph_dp, info, pub_opp, k_mac, token_opp, ssc)) {
        r = SC_ERROR_INTERNAL;
        debug_ossl(ctx);
        goto err;
    }

    /* XXX parse CHAT to check role of terminal */

err:
    if (info)
        PACEInfo_free(info);
    if (static_dp)
        PACEDomainParameterInfo_clear_free(static_dp);
    if (eph_dp)
        PACEDomainParameterInfo_clear_free(eph_dp);
    if (enc_nonce)
        BUF_MEM_free(enc_nonce);
    if (nonce) {
        OPENSSL_cleanse(nonce->data, nonce->length);
        BUF_MEM_free(nonce);
    }
    if (mdata)
        BUF_MEM_free(mdata);
    if (mdata_opp)
        BUF_MEM_free(mdata_opp);
    if (k_enc) {
        OPENSSL_cleanse(k_enc->data, k_enc->length);
        BUF_MEM_free(k_enc);
    }
    if (k_mac) {
        OPENSSL_cleanse(k_mac->data, k_mac->length);
        BUF_MEM_free(k_mac);
    }
    if (token_opp)
        BUF_MEM_free(token_opp);
    if (token)
        BUF_MEM_free(token);
    if (pub)
        BUF_MEM_free(pub);
    if (pub_opp)
        BUF_MEM_free(pub_opp);
    if (key) {
        OPENSSL_cleanse(key->data, key->length);
        BUF_MEM_free(key);
    }
    if (sec)
        PACE_SEC_clean_free(sec);
    if (pctx)
        PACE_CTX_clear_free(pctx);

    SC_FUNC_RETURN(ctx, SC_LOG_TYPE_DEBUG, r);
}
#endif