/*
 * This is the actual card emulator.
 *
 * These functions can be implemented in different ways on different platforms
 * using the underlying system primitives. For Linux it uses NSS, though direct
 * to PKCS #11, openssl+pkcs11, or even gnu crypto libraries+pkcs #11 could be
 * used. On Windows CAPI could be used.
 *
 * This work is licensed under the terms of the GNU LGPL, version 2.1 or later.
 * See the COPYING file in the top-level directory.
 */
#include "config.h"

#include <glib.h>

#include "common.h"

/*
 * NSS headers
 */

/* avoid including prototypes.h that redefines uint32 */
#define NO_NSPR_10_SUPPORT

#include <nss.h>
#include <pk11pub.h>
#include <cert.h>
#include <keyhi.h>
#include <secmod.h>
#include <prthread.h>
#include <secerr.h>
#include <secoid.h>
#include <secmodt.h>
#include <sechash.h>

#include "vcard.h"
#include "card_7816t.h"
#include "vcard_emul.h"
#include "vreader.h"
#include "vevent.h"

#include "vcardt_internal.h"
#if defined(ENABLE_PCSC)
#include "capcsc.h"
#endif


typedef enum {
    VCardEmulUnknown = -1,
    VCardEmulFalse = 0,
    VCardEmulTrue = 1
} VCardEmulTriState;

struct VCardKeyStruct {
    CERTCertificate *cert;
    PK11SlotInfo *slot;
    VCardEmulTriState failedX509;
};


typedef struct VirtualReaderOptionsStruct VirtualReaderOptions;

struct VReaderEmulStruct {
    PK11SlotInfo *slot;
    VCardEmulType default_type;
    char *type_params;
    PRBool present;
    int     series;
    VCard *saved_vcard;
};

/*
 *  NSS Specific options
 */
struct VirtualReaderOptionsStruct {
    char *name;
    char *vname;
    VCardEmulType card_type;
    char *type_params;
    char **cert_name;
    int cert_count;
};

enum {
    USE_HW_NO,
    USE_HW_YES,
    USE_HW_REMOVABLE,
};

struct VCardEmulOptionsStruct {
    char *nss_db;
    VirtualReaderOptions *vreader;
    int vreader_count;
    VCardEmulType hw_card_type;
    char *hw_type_params;
    int use_hw;
};

static int nss_emul_init;

/* if we have more that just the slot, define
 * VCardEmulStruct here */

/*
 * allocate the set of arrays for certs, cert_len, key
 */
static void
vcard_emul_alloc_arrays(unsigned char ***certsp, int **cert_lenp,
                        VCardKey ***keysp, int cert_count)
{
    *certsp = g_new(unsigned char *, cert_count);
    *cert_lenp = g_new(int, cert_count);
    *keysp = g_new(VCardKey *, cert_count);
}

/*
 * Emulator specific card information
 */
typedef struct CardEmulCardStruct CardEmulPrivate;

static VCardEmul *
vcard_emul_new_card(PK11SlotInfo *slot)
{
    PK11_ReferenceSlot(slot);
    /* currently we don't need anything other than the slot */
    return (VCardEmul *)slot;
}

static void
vcard_emul_delete_card(VCardEmul *vcard_emul)
{
    PK11SlotInfo *slot = (PK11SlotInfo *)vcard_emul;
    if (slot == NULL) {
        return;
    }
    PK11_FreeSlot(slot);
}

static PK11SlotInfo *
vcard_emul_card_get_slot(VCard *card)
{
    /* note, the card is holding the reference, no need to get another one */
    return (PK11SlotInfo *)vcard_get_private(card);
}


/*
 * key functions
 */
/* private constructure */
static VCardKey *
vcard_emul_make_key(PK11SlotInfo *slot, CERTCertificate *cert)
{
    VCardKey *key;

    key = g_new(VCardKey, 1);
    key->slot = PK11_ReferenceSlot(slot);
    key->cert = CERT_DupCertificate(cert);
    key->failedX509 = VCardEmulUnknown;
    return key;
}

/* destructor */
void
vcard_emul_delete_key(VCardKey *key)
{
    if (!nss_emul_init || (key == NULL)) {
        return;
    }
    if (key->cert) {
        CERT_DestroyCertificate(key->cert);
    }
    if (key->slot) {
        PK11_FreeSlot(key->slot);
    }
    g_free(key);
}

/*
 * grab the nss key from a VCardKey. If it doesn't exist, try to look it up
 */
static SECKEYPrivateKey *
vcard_emul_get_nss_key(VCardKey *key)
{
    /* NOTE: if we aren't logged into the token, this could return NULL */
    return PK11_FindPrivateKeyFromCert(key->slot, key->cert, NULL);
}

/*
 * Map NSS errors to 7816 errors
 */
static vcard_7816_status_t
vcard_emul_map_error(int error)
{
    switch (error) {
    case SEC_ERROR_TOKEN_NOT_LOGGED_IN:
        return VCARD7816_STATUS_ERROR_CONDITION_NOT_SATISFIED;
    case SEC_ERROR_BAD_DATA:
    case SEC_ERROR_OUTPUT_LEN:
    case SEC_ERROR_INPUT_LEN:
    case SEC_ERROR_INVALID_ARGS:
    case SEC_ERROR_INVALID_ALGORITHM:
    case SEC_ERROR_NO_KEY:
    case SEC_ERROR_INVALID_KEY:
    case SEC_ERROR_DECRYPTION_DISALLOWED:
    case SEC_ERROR_PKCS11_GENERAL_ERROR:
        return VCARD7816_STATUS_ERROR_DATA_INVALID;
    case SEC_ERROR_NO_MEMORY:
        return VCARD7816_STATUS_EXC_ERROR_MEMORY_FAILURE;
    default:
        g_debug("error %x", 0x2000 + error);
        g_warn_if_reached();
    }
    return VCARD7816_STATUS_EXC_ERROR_CHANGE;
}

/* get RSA bits */
int
vcard_emul_rsa_bits(VCardKey *key)
{
    SECKEYPublicKey *pub_key;
    int bits = -1;

    if (key == NULL) {
        /* couldn't get the key, indicate that we aren't logged in */
        return -1;
    }
    pub_key = CERT_ExtractPublicKey(key->cert);
    if (pub_key == NULL) {
        /* couldn't get the key, indicate that we aren't logged in */
        return -1;
    }

    bits = SECKEY_PublicKeyStrengthInBits(pub_key);
    SECKEY_DestroyPublicKey(pub_key);
    return bits;
}

/* RSA sign/decrypt with the key, signature happens 'in place' */
vcard_7816_status_t
vcard_emul_rsa_op(VCard *card, VCardKey *key,
                  unsigned char *buffer, int buffer_size)
{
    SECKEYPrivateKey *priv_key;
    unsigned signature_len;
    PK11SlotInfo *slot;
    SECStatus rv;
    unsigned char buf[2048];
    unsigned char *bp = NULL;
    int pad_len;
    vcard_7816_status_t ret = VCARD7816_STATUS_SUCCESS;

    assert(buffer_size >= 0);
    if ((!nss_emul_init) || (key == NULL)) {
        /* couldn't get the key, indicate that we aren't logged in */
        return VCARD7816_STATUS_ERROR_CONDITION_NOT_SATISFIED;
    }
    priv_key = vcard_emul_get_nss_key(key);
    if (priv_key == NULL) {
        /* couldn't get the key, indicate that we aren't logged in */
        return VCARD7816_STATUS_ERROR_CONDITION_NOT_SATISFIED;
    }
    slot = vcard_emul_card_get_slot(card);

    /*
     * this is only true of the rsa signature
     */
    signature_len = PK11_SignatureLen(priv_key);
    if ((unsigned)buffer_size != signature_len) {
        ret = VCARD7816_STATUS_ERROR_DATA_INVALID;
        goto cleanup;
    }
    /* be able to handle larger keys if necessary */
    bp = &buf[0];
    if (sizeof(buf) < signature_len) {
        bp = g_malloc(signature_len);
    }

    /*
     * do the raw operations. Some tokens claim to do CKM_RSA_X_509, but then
     * choke when they try to do the actual operations. Try to detect
     * those cases and treat them as if the token didn't claim support for
     * X_509.
     */
    if (key->failedX509 != VCardEmulTrue
                              && PK11_DoesMechanism(slot, CKM_RSA_X_509)) {
        rv = PK11_PrivDecryptRaw(priv_key, bp, &signature_len, signature_len,
                                 buffer, buffer_size);
        if (rv == SECSuccess) {
            assert((unsigned)buffer_size == signature_len);
            memcpy(buffer, bp, signature_len);
            key->failedX509 = VCardEmulFalse;
            goto cleanup;
        }
        /*
         * we've had a successful X509 operation, this failure must be
         * something else
         */
        if (key->failedX509 == VCardEmulFalse) {
            ret = vcard_emul_map_error(PORT_GetError());
            goto cleanup;
        }
        /*
         * key->failedX509 must be Unknown at this point, try the
         * non-x_509 case
         */
    }
    /* token does not support CKM_RSA_X509, emulate that with CKM_RSA_PKCS */
    /* is this a PKCS #1 formatted signature? */
    if ((buffer[0] == 0) && (buffer[1] == 1)) {
        int i;

        for (i = 2; i < buffer_size; i++) {
            /* rsa signature pad */
            if (buffer[i] != 0xff) {
                break;
            }
        }
        if ((i < buffer_size) && (buffer[i] == 0)) {
            /* yes, we have a properly formatted PKCS #1 signature */
            /*
             * NOTE: even if we accidentally got an encrypt buffer, which
             * through sheer luck started with 00, 01, ff, 00, it won't matter
             * because the resulting Sign operation will effectively decrypt
             * the real buffer.
             */
            SECItem signature;
            SECItem hash;

            i++;
            hash.data = &buffer[i];
            hash.len = buffer_size - i;
            signature.data = bp;
            signature.len = signature_len;
            rv = PK11_Sign(priv_key,  &signature, &hash);
            if (rv != SECSuccess) {
                ret = vcard_emul_map_error(PORT_GetError());
                goto cleanup;
            }
            assert((unsigned)buffer_size == signature.len);
            memcpy(buffer, bp, signature.len);
            /*
             * we got here because either the X509 attempt failed, or the
             * token couldn't do the X509 operation, in either case stay
             * with the PKCS version for future operations on this key
             */
            key->failedX509 = VCardEmulTrue;
            goto cleanup;
        }
    }
    /* We can not do raw RSA operation and the bytes do not look like PKCS#1.5
     * Assuming it is deciphering operation.
     */
    rv = PK11_PrivDecryptPKCS1(priv_key, bp, &signature_len, buffer_size, buffer, buffer_size);
    if (rv != SECSuccess) {
        /* The assumption was wrong. Give up */
        ret = vcard_emul_map_error(PORT_GetError());
        goto cleanup;
    }
    pad_len = buffer_size - signature_len;
    if (pad_len < 4) {
        ret = VCARD7816_STATUS_ERROR_GENERAL;
        goto cleanup;
    }
    /*
     * OK now we've decrypted the payload, package it up in PKCS #1 for the
     * upper layer.
     */
    buffer[0] = 0;
    buffer[1] = 2; /* RSA_encrypt  */
    pad_len -= 3; /* format is 0 || 2 || pad || 0 || data */
    /*
     * padding for PKCS #1 encrypted data is a string of random bytes. The
     * random bytes protect against potential decryption attacks against RSA.
     * Since PrivDecrypt has already stripped those bytes, we can't reconstruct
     * them. This shouldn't matter to the upper level code which should just
     * strip this code out anyway, so We'll pad with a constant 3.
     */
    memset(&buffer[2], 0x03, pad_len);
    pad_len += 2; /* index to the end of the pad */
    buffer[pad_len] = 0;
    pad_len++; /* index to the start of the data */
    memcpy(&buffer[pad_len], bp, signature_len);
    /*
     * we got here because either the X509 attempt failed, or the
     * token couldn't do the X509 operation, in either case stay
     * with the PKCS version for future operations on this key
     */
    key->failedX509 = VCardEmulTrue;
cleanup:
    if (bp != buf) {
        g_free(bp);
    }
    SECKEY_DestroyPrivateKey(priv_key);
    return ret;
}

/*
 * Login functions
 */
/* return the number of login attempts still possible on the card. if unknown,
 * return -1 */
int
vcard_emul_get_login_count(G_GNUC_UNUSED VCard *card)
{
    return -1;
}

/* login into the card, return the 7816 status word (sw2 || sw1) */
vcard_7816_status_t
vcard_emul_login(VCard *card, unsigned char *pin, int pin_len)
{
    PK11SlotInfo *slot;
    unsigned char *pin_string;
    int i;
    SECStatus rv;

    if (!nss_emul_init) {
        return VCARD7816_STATUS_ERROR_CONDITION_NOT_SATISFIED;
    }
    slot = vcard_emul_card_get_slot(card);
     /* We depend on the PKCS #11 module internal login state here because we
      * create a separate process to handle each guest instance. If we needed
      * to handle multiple guests from one process, then we would need to keep
      * a lot of extra state in our card structure
      * */
    pin_string = g_malloc(pin_len+1);
    memcpy(pin_string, pin, pin_len);
    pin_string[pin_len] = 0;

    /* handle CAC expanded pins correctly */
    for (i = pin_len-1; i >= 0 && (pin_string[i] == 0xff); i--) {
        pin_string[i] = 0;
    }

    /* If using an emulated card, make sure to log out of any already logged in
     * session. */
    vcard_emul_logout(card);

    rv = PK11_Authenticate(slot, PR_FALSE, pin_string);
    memset(pin_string, 0, pin_len);  /* don't let the pin hang around in memory
                                        to be snooped */
    g_free(pin_string);
    if (rv == SECSuccess) {
        return VCARD7816_STATUS_SUCCESS;
    }
    /* map the error from port get error */
    return VCARD7816_STATUS_ERROR_CONDITION_NOT_SATISFIED;
}

int
vcard_emul_is_logged_in(VCard *card)
{
    PK11SlotInfo *slot;

    if (!nss_emul_init) {
        return VCARD7816_STATUS_ERROR_CONDITION_NOT_SATISFIED;
    }

    slot = vcard_emul_card_get_slot(card);
     /* We depend on the PKCS #11 module internal login state here because we
      * create a separate process to handle each guest instance. If we needed
      * to handle multiple guests from one process, then we would need to keep
      * a lot of extra state in our card structure
      */

    /* If we do not need log in, we present the token as "logged in" */
    if (PK11_NeedLogin(slot) == PR_FALSE) {
        return 1;
    }

    /* For the tokens that require login, delegate to NSS to figure out the
     * login status */
    return !!PK11_IsLoggedIn(slot, NULL);
}

void
vcard_emul_logout(VCard *card)
{
    PK11SlotInfo *slot;

    if (!nss_emul_init) {
        return;
    }

    slot = vcard_emul_card_get_slot(card);
    if (PK11_IsLoggedIn(slot, NULL)) {
        PK11_Logout(slot); /* NOTE: ignoring SECStatus return value */
    }
}

void
vcard_emul_reset(VCard *card, G_GNUC_UNUSED VCardPower power)
{
    /*
     * if we reset the card (either power on or power off), we lose our login
     * state
     */
    vcard_emul_logout(card);

    /* TODO: we may also need to send insertion/removal events? */
}

static VReader *
vcard_emul_find_vreader_from_slot(PK11SlotInfo *slot)
{
    VReaderList *reader_list = vreader_get_reader_list();
    VReaderListEntry *current_entry;

    if (reader_list == NULL) {
        return NULL;
    }
    for (current_entry = vreader_list_get_first(reader_list); current_entry;
                        current_entry = vreader_list_get_next(current_entry)) {
        VReader *reader = vreader_list_get_reader(current_entry);
        VReaderEmul *reader_emul = vreader_get_private(reader);
        if (reader_emul->slot == slot) {
            vreader_list_delete(reader_list);
            return reader;
        }
        vreader_free(reader);
    }

    vreader_list_delete(reader_list);
    return NULL;
}

/*
 * create a new reader emul
 */
static VReaderEmul *
vreader_emul_new(PK11SlotInfo *slot, VCardEmulType type, const char *params)
{
    VReaderEmul *new_reader_emul;

    new_reader_emul = g_new(VReaderEmul, 1);

    new_reader_emul->slot = PK11_ReferenceSlot(slot);
    new_reader_emul->default_type = type;
    new_reader_emul->type_params = g_strdup(params);
    new_reader_emul->present = PR_FALSE;
    new_reader_emul->series = 0;
    new_reader_emul->saved_vcard = NULL;
    return new_reader_emul;
}

static void
vreader_emul_delete(VReaderEmul *vreader_emul)
{
    if (vreader_emul == NULL) {
        return;
    }
    vcard_free(vreader_emul->saved_vcard);
    if (vreader_emul->slot) {
        PK11_FreeSlot(vreader_emul->slot);
    }
    g_free(vreader_emul->type_params);
    g_free(vreader_emul);
}

/*
 *  TODO: move this to emulater non-specific file
 */
static VCardEmulType
vcard_emul_get_type(VReader *vreader)
{
    VReaderEmul *vreader_emul;

    vreader_emul = vreader_get_private(vreader);
    if (vreader_emul && vreader_emul->default_type != VCARD_EMUL_NONE) {
        return vreader_emul->default_type;
    }

    return vcard_emul_type_select(vreader);
}
/*
 *  TODO: move this to emulater non-specific file
 */
static const char *
vcard_emul_get_type_params(VReader *vreader)
{
    VReaderEmul *vreader_emul;

    vreader_emul = vreader_get_private(vreader);
    if (vreader_emul && vreader_emul->type_params) {
        return vreader_emul->type_params;
    }

    return "";
}

/* pull the slot out of the reader private data */
static PK11SlotInfo *
vcard_emul_reader_get_slot(VReader *vreader)
{
    VReaderEmul *vreader_emul = vreader_get_private(vreader);
    if (vreader_emul == NULL) {
        return NULL;
    }
    return vreader_emul->slot;
}

/*
 *  Card ATR's map to physical cards. vcard_alloc_atr will set appropriate
 *  historical bytes for any software emulated card. The remaining bytes can be
 *  used to indicate the actual emulator
 */
static unsigned char *nss_atr;
static int nss_atr_len;

void
vcard_emul_get_atr(G_GNUC_UNUSED VCard *card, unsigned char *atr, int *atr_len)
{
    int len;
    assert(atr != NULL);

    if (nss_atr == NULL) {
        nss_atr = vcard_alloc_atr("NSS", &nss_atr_len);
    }
    len = MIN(nss_atr_len, *atr_len);
    memcpy(atr, nss_atr, len);
    *atr_len = len;
}

static SECStatus
vcard_emul_create_serial(VCard *card, unsigned char *data, int len)
{
    HASH_HashType hashType;
    HASHContext *hashContext = NULL;
    unsigned char digest[32];
    unsigned int digestLen = 0;

    hashType = HASH_GetHashTypeByOidTag(SEC_OID_SHA256);
    hashContext = HASH_Create(hashType);
    if (hashContext == NULL) {
        return SECFailure;
    }

    HASH_Begin(hashContext);
    HASH_Update(hashContext, data, len);
    HASH_End(hashContext, digest, &digestLen, 32);
    HASH_Destroy(hashContext);

    vcard_set_serial(card, digest, (size_t) digestLen);

    return SECSuccess;
}

/*
 * create a new card from certs and keys
 */
static VCard *
vcard_emul_make_card(VReader *reader,
                     unsigned char * const *certs, int *cert_len,
                     VCardKey *keys[], int cert_count)
{
    VCardEmul *vcard_emul;
    VCard *vcard;
    PK11SlotInfo *slot;
    VCardEmulType type;
    const char *params;

    g_debug("%s: called", __func__);

    type = vcard_emul_get_type(reader);

    /* ignore the inserted card */
    if (type == VCARD_EMUL_NONE) {
        return NULL;
    }
    slot = vcard_emul_reader_get_slot(reader);
    if (slot == NULL) {
        return NULL;
    }

    params = vcard_emul_get_type_params(reader);
    /* params these can be NULL */

    vcard_emul = vcard_emul_new_card(slot);
    if (vcard_emul == NULL) {
        return NULL;
    }
    vcard = vcard_new(vcard_emul, vcard_emul_delete_card);
    if (vcard == NULL) {
        vcard_emul_delete_card(vcard_emul);
        return NULL;
    }

    if (cert_count > 0) {
        vcard_emul_create_serial(vcard, certs[0], cert_len[0]);
    }

    vcard_init(reader, vcard, type, params, certs, cert_len, keys, cert_count);
    return vcard;
}


/*
 * 'clone' a physical card as a virtual card
 */
static VCard *
vcard_emul_mirror_card(VReader *vreader)
{
    /*
     * lookup certs using the C_FindObjects. The Stan Cert handle won't give
     * us the real certs until we log in.
     */
    PK11GenericObject *firstObj, *thisObj;
    int cert_count, i;
    unsigned char **certs;
    SECItem **ids;
    int *cert_len;
    VCardKey **keys;
    PK11SlotInfo *slot;
    VCard *card;

    g_debug("%s: called", __func__);

    slot = vcard_emul_reader_get_slot(vreader);
    if (slot == NULL) {
        return NULL;
    }

    firstObj = PK11_FindGenericObjects(slot, CKO_CERTIFICATE);
    if (firstObj == NULL) {
        return NULL;
    }

    /* count the certs */
    cert_count = 0;
    for (thisObj = firstObj; thisObj;
                             thisObj = PK11_GetNextGenericObject(thisObj)) {
        cert_count++;
    }

    /* allocate the arrays */
    vcard_emul_alloc_arrays(&certs, &cert_len, &keys, cert_count);
    ids = g_new(SECItem *, cert_count);

    /* fill in the arrays */
    cert_count = i = 0;
    for (thisObj = firstObj; thisObj;
                             thisObj = PK11_GetNextGenericObject(thisObj)) {
        SECItem derCert, *id;
        CERTCertificate *cert;
        SECStatus rv;

        g_debug("%s: Found certificate", __func__);
        rv = PK11_ReadRawAttribute(PK11_TypeGeneric, thisObj,
                                   CKA_VALUE, &derCert);
        if (rv != SECSuccess) {
            continue;
        }
        /* Read ID and try to sort by this to get reproducible results
         * in case of underlying pkcs11 module does not provide it */
        id = SECITEM_AllocItem(NULL, NULL, 0);
        rv = PK11_ReadRawAttribute(PK11_TypeGeneric, thisObj, CKA_ID, id);
        if (rv != SECSuccess) {
            SECITEM_FreeItem(&derCert, PR_FALSE);
            SECITEM_FreeItem(id, PR_TRUE);
            continue;
        }
        /* create floating temp cert. This gives us a cert structure even if
         * the token isn't logged in */
        cert = CERT_NewTempCertificate(CERT_GetDefaultCertDB(), &derCert,
                                       NULL, PR_FALSE, PR_TRUE);
        SECITEM_FreeItem(&derCert, PR_FALSE);
        if (cert == NULL) {
            SECITEM_FreeItem(id, PR_TRUE);
            continue;
        }

        for (i = 0; i < cert_count; i++) {
            if (SECITEM_CompareItem(id, ids[i]) < SECEqual) {
                /* Make space for the item here, move the rest of the items */
                memmove(&certs[i + 1], &certs[i], (cert_count - i) * sizeof(certs[0]));
                memmove(&cert_len[i + 1], &cert_len[i], (cert_count - i) * sizeof(cert_len[0]));
                memmove(&keys[i + 1], &keys[i], (cert_count - i) * sizeof(keys[0]));
                memmove(&ids[i + 1], &ids[i], (cert_count - i) * sizeof(ids[0]));
                break;
            }
        }
        certs[i] = cert->derCert.data;
        cert_len[i] = cert->derCert.len;
        keys[i] = vcard_emul_make_key(slot, cert);
        ids[i] = id;
        cert_count++;
        CERT_DestroyCertificate(cert); /* key obj still has a reference */
    }
    PK11_DestroyGenericObjects(firstObj);
    /* No longer needed */
    for (i = 0; i < cert_count; i++) {
        SECITEM_FreeItem(ids[i], PR_TRUE);
    }
    g_free(ids);

    /* now create the card */
    card = vcard_emul_make_card(vreader, certs, cert_len, keys, cert_count);
    g_free(certs);
    g_free(cert_len);
    g_free(keys);

    return card;
}

static VCardEmulType default_card_type = VCARD_EMUL_NONE;
static const char *default_type_params = "";

/*
 * This thread looks for card and reader insertions and puts events on the
 * event queue
 */
static void
vcard_emul_event_thread(void *arg)
{
    PK11SlotInfo *slot;
    VReader *vreader;
    VReaderEmul *vreader_emul;
    VCard *vcard;
    SECMODModule *module = (SECMODModule *)arg;

    do {
        /*
         * XXX - the latency value doesn't matter one bit. you only get no
         * blocking (flags |= CKF_DONT_BLOCK) or PKCS11_WAIT_LATENCY (==500),
         * hard coded in coolkey.  And it isn't coolkey's fault - the timeout
         * value we pass get's dropped on the floor before C_WaitForSlotEvent
         * is called.
         */
        slot = SECMOD_WaitForAnyTokenEvent(module, 0, 500);
        if (slot == NULL) {
            /* this could be just a no event indication */
            if (PORT_GetError() == SEC_ERROR_NO_EVENT) {
                continue;
            }
            break;
        }
        vreader = vcard_emul_find_vreader_from_slot(slot);
        if (vreader == NULL) {
            /* new vreader */
            vreader_emul = vreader_emul_new(slot, default_card_type,
                                            default_type_params);
            vreader = vreader_new(PK11_GetSlotName(slot), vreader_emul,
                                  vreader_emul_delete);
            PK11_FreeSlot(slot);
            slot = NULL;
            vreader_add_reader(vreader);
            vreader_free(vreader);
            continue;
        }
        /* card remove/insert */
        vreader_emul = vreader_get_private(vreader);
        if (PK11_IsPresent(slot)) {
            int series = PK11_GetSlotSeries(slot);
            if (series != vreader_emul->series) {
                if (vreader_emul->present) {
                    vreader_insert_card(vreader, NULL);
                }
                vcard = vcard_emul_mirror_card(vreader);
                vreader_insert_card(vreader, vcard);
                vcard_free(vcard);
            }
            vreader_emul->series = series;
            vreader_emul->present = 1;
            vreader_free(vreader);
            PK11_FreeSlot(slot);
            continue;
        }
        if (vreader_emul->present) {
            vreader_insert_card(vreader, NULL);
        }
        vreader_emul->series = 0;
        vreader_emul->present = 0;
        PK11_FreeSlot(slot);
        vreader_free(vreader);
    } while (1);
}

/* if the card is inserted when we start up, make sure our state is correct */
static void
vcard_emul_init_series(VReader *vreader, G_GNUC_UNUSED VCard *vcard)
{
    VReaderEmul *vreader_emul = vreader_get_private(vreader);
    PK11SlotInfo *slot = vreader_emul->slot;

    vreader_emul->present = PK11_IsPresent(slot);
    vreader_emul->series = PK11_GetSlotSeries(slot);
    if (vreader_emul->present == 0) {
        vreader_insert_card(vreader, NULL);
    }
}

/*
 * each module has a separate wait call, create a thread for each module that
 * we are using.
 */
static void
vcard_emul_new_event_thread(SECMODModule *module)
{
    PR_CreateThread(PR_SYSTEM_THREAD, vcard_emul_event_thread,
                     module, PR_PRIORITY_HIGH, PR_GLOBAL_THREAD,
                     PR_UNJOINABLE_THREAD, 0);
}

static const VCardEmulOptions default_options = {
    .nss_db = NULL,
    .vreader = NULL,
    .vreader_count = 0,
    .hw_card_type = VCARD_EMUL_CAC,
    .hw_type_params = NULL,
    .use_hw = USE_HW_YES,
};


/*
 *  NSS needs the app to supply a password prompt. In our case the only time
 *  the password is supplied is as part of the Login APDU. The actual password
 *  is passed in the pw_arg in that case. In all other cases pw_arg should be
 *  NULL.
 */
static char *
vcard_emul_get_password(G_GNUC_UNUSED PK11SlotInfo *slot, PRBool retries, void *pw_arg)
{
    /* if it didn't work the first time, don't keep trying */
    if (retries) {
        return NULL;
    }
    /* we are looking up a password when we don't have one in hand */
    if (pw_arg == NULL) {
        return NULL;
    }
    /* TODO: we really should verify that were are using the right slot */
    return PORT_Strdup(pw_arg);
}

/* Force a card removal even if the card is not physically removed */
VCardEmulError
vcard_emul_force_card_remove(VReader *vreader)
{
    if (!nss_emul_init || (vreader_card_is_present(vreader) != VREADER_OK)) {
        return VCARD_EMUL_FAIL; /* card is already removed */
    }

    /* OK, remove it */
    vreader_insert_card(vreader, NULL);
    return VCARD_EMUL_OK;
}

/* Re-insert of a card that has been removed by force removal */
VCardEmulError
vcard_emul_force_card_insert(VReader *vreader)
{
    VReaderEmul *vreader_emul;
    VCard *vcard;

    if (!nss_emul_init || (vreader_card_is_present(vreader) == VREADER_OK)) {
        return VCARD_EMUL_FAIL; /* card is already removed */
    }
    vreader_emul = vreader_get_private(vreader);

    /* if it's a softcard, get the saved vcard from the reader emul structure */
    if (vreader_emul->saved_vcard) {
        vcard = vcard_reference(vreader_emul->saved_vcard);
    } else {
        /* it must be a physical card, rebuild it */
        if (!PK11_IsPresent(vreader_emul->slot)) {
            /* physical card has been removed, not way to reinsert it */
            return VCARD_EMUL_FAIL;
        }
        vcard = vcard_emul_mirror_card(vreader);
    }
    vreader_insert_card(vreader, vcard);
    vcard_free(vcard);

    return VCARD_EMUL_OK;
}

/* Previously we returned FAIL if no readers found. This makes
 * no sense when using hardware, since there may be no readers connected
 * at the time vcard_emul_init is called, but they will be properly
 * recognized later. So Instead return FAIL only if no_hw==1 and no
 * vcards can be created (indicates error with certificates provided
 * or db), or if any other higher level error (NSS error, missing coolkey). */
static int vcard_emul_init_called;
static NSSInitContext *nss_ctx = NULL;

VCardEmulError
vcard_emul_init(const VCardEmulOptions *options)
{
    PRBool has_readers = PR_FALSE;
    VReader *vreader;
    VReaderEmul *vreader_emul;
    SECMODListLock *module_lock;
    SECMODModuleList *module_list;
    SECMODModuleList *mlp;
    int i;
    gchar *path = NULL;
    const gchar *nss_db;

    g_debug("%s: called", __func__);

    if (vcard_emul_init_called) {
        return VCARD_EMUL_INIT_ALREADY_INITED;
    }
    vcard_emul_init_called = 1;
    vreader_init();
    vevent_queue_init();

    if (options == NULL) {
        options = &default_options;
    }

#if defined(ENABLE_PCSC)
    if (options->use_hw && options->hw_card_type == VCARD_EMUL_PASSTHRU) {
        if (options->vreader_count > 0) {
            fprintf(stderr, "Error: you cannot use a soft card and "
                            "a passthru card simultaneously.\n");
            return VCARD_EMUL_FAIL;
        }

        if (capcsc_init()) {
            fprintf(stderr, "Error initializing PCSC interface.\n");
            return VCARD_EMUL_FAIL;
        }

        g_debug("%s: returning with passthrough initialized", __func__);
        return VCARD_EMUL_OK;
    }
#endif

    /* first initialize NSS */
    nss_db = options->nss_db;
    if (nss_db == NULL) {
#ifndef _WIN32
        nss_db = "/etc/pki/nssdb";
#else
        const gchar * const *config_dirs = g_get_system_config_dirs();
        if (config_dirs == NULL || config_dirs[0] == NULL) {
            return VCARD_EMUL_FAIL;
        }

        path = g_build_filename(config_dirs[0], "pki", "nssdb", NULL);
        nss_db = path;
#endif
    }

    nss_ctx = NSS_InitContext(nss_db, "", "", "", NULL, NSS_INIT_READONLY);
    if (nss_ctx == NULL) {
        g_debug("%s: NSS_InitContext failed. Does the DB directory '%s' exist?",
                __func__, nss_db);
        g_free(path);
        return VCARD_EMUL_FAIL;
    }
    g_free(path);
    path = NULL;

    /* Set password callback function */
    PK11_SetPasswordFunc(vcard_emul_get_password);

    /* set up soft cards emulated by software certs rather than physical cards
     * */
    for (i = 0; i < options->vreader_count; i++) {
        int j;
        int cert_count;
        unsigned char **certs;
        int *cert_len;
        VCardKey **keys;
        PK11SlotInfo *slot;

        slot = PK11_FindSlotByName(options->vreader[i].name);
        if (slot == NULL) {
            continue;
        }
        vreader_emul = vreader_emul_new(slot, options->vreader[i].card_type,
                                        options->vreader[i].type_params);
        vreader = vreader_new(options->vreader[i].vname, vreader_emul,
                              vreader_emul_delete);
        vreader_add_reader(vreader);

        vcard_emul_alloc_arrays(&certs, &cert_len, &keys,
                                options->vreader[i].cert_count);

        cert_count = 0;
        for (j = 0; j < options->vreader[i].cert_count; j++) {
            /* we should have a better way of identifying certs than by
             * nickname here */
            CERTCertificate *cert = PK11_FindCertFromNickname(
                                        options->vreader[i].cert_name[j],
                                        NULL);
            if (cert == NULL) {
                continue;
            }
            certs[cert_count] = cert->derCert.data;
            cert_len[cert_count] = cert->derCert.len;
            keys[cert_count] = vcard_emul_make_key(slot, cert);
            /* this is safe because the key is still holding a cert reference */
            CERT_DestroyCertificate(cert);
            cert_count++;
        }
        if (cert_count) {
            VCard *vcard = vcard_emul_make_card(vreader, certs, cert_len,
                                                keys, cert_count);
            vreader_insert_card(vreader, vcard);
            vcard_emul_init_series(vreader, vcard);
            /* allow insertion and removal of soft cards */
            vreader_emul->saved_vcard = vcard_reference(vcard);
            vcard_free(vcard);
            vreader_free(vreader);
            has_readers = PR_TRUE;
        }
        PK11_FreeSlot(slot);
        g_free(certs);
        g_free(cert_len);
        g_free(keys);
    }

    /* if we aren't suppose to use hw, skip looking up hardware tokens */
    if (!options->use_hw) {
        nss_emul_init = has_readers;
        g_debug("%s: returning: Not using HW", __func__);
        return has_readers ? VCARD_EMUL_OK : VCARD_EMUL_FAIL;
    }

    /* make sure we have some PKCS #11 module loaded */
    module_lock = SECMOD_GetDefaultModuleListLock();
    module_list = SECMOD_GetDefaultModuleList();

    /* now examine all the slots, finding which should be readers */
    /* We should control this with options. For now we mirror out any
     * removable hardware slot */
    default_card_type = options->hw_card_type;
    default_type_params = g_strdup(options->hw_type_params);

    SECMOD_GetReadLock(module_lock);
    for (mlp = module_list; mlp; mlp = mlp->next) {
        SECMODModule *module = mlp->module;

        /* Ignore the internal module */
        if (module == NULL || module == SECMOD_GetInternalModule()) {
            continue;
        }

        g_debug("%s: Listing modules, trying %s", __func__, module->commonName);
        for (i = 0; i < module->slotCount; i++) {
            PK11SlotInfo *slot = module->slots[i];

            /* only map removable HW slots */
            if (slot == NULL || !PK11_IsRemovable(slot) ||
                (options->use_hw == USE_HW_YES && !PK11_IsHW(slot))) {
                continue;
            }
            if (strcmp("E-Gate 0 0", PK11_GetSlotName(slot)) == 0) {
                /*
                 * coolkey <= 1.1.0-20 emulates this reader if it can't find
                 * any hardware readers. This causes problems, warn user of
                 * problems.
                 */
                fprintf(stderr, "known bad coolkey version - see "
                        "https://bugzilla.redhat.com/show_bug.cgi?id=802435\n");
                continue;
            }
            vreader_emul = vreader_emul_new(slot, options->hw_card_type,
                                            options->hw_type_params);
            vreader = vreader_new(PK11_GetSlotName(slot), vreader_emul,
                                  vreader_emul_delete);
            vreader_add_reader(vreader);
            g_debug("%s: Added reader from slot %s", __func__,
                    PK11_GetSlotName(slot));

            if (PK11_IsPresent(slot)) {
                VCard *vcard;
                vcard = vcard_emul_mirror_card(vreader);
                vreader_insert_card(vreader, vcard);
                vcard_emul_init_series(vreader, vcard);
                g_debug("%s: Added card to the reader %s", __func__,
                        vreader_get_name(vreader));
                vcard_free(vcard);
            }
            vreader_free(vreader);
        }
        vcard_emul_new_event_thread(module);
    }
    SECMOD_ReleaseReadLock(module_lock);
    nss_emul_init = PR_TRUE;

    return VCARD_EMUL_OK;
}

/* Recreate card insert events for all readers (user should
 * deduce implied reader insert. perhaps do a reader insert as well?)
 */
void
vcard_emul_replay_insertion_events(void)
{
    VReaderListEntry *current_entry;
    VReaderListEntry *next_entry;
    VReaderList *list = vreader_get_reader_list();

    for (current_entry = vreader_list_get_first(list); current_entry;
            current_entry = next_entry) {
        VReader *vreader = vreader_list_get_reader(current_entry);
        next_entry = vreader_list_get_next(current_entry);
        vreader_queue_card_event(vreader);
    }

    vreader_list_delete(list);
}

VCardEmulError
vcard_emul_finalize(void)
{
    SECStatus rv;

    rv = NSS_ShutdownContext(nss_ctx);
    if (rv != SECSuccess) {
        g_debug("%s: NSS_ShutdownContext failed.", __func__);
        return VCARD_EMUL_FAIL;
    }
    nss_ctx = NULL;

    return VCARD_EMUL_OK;
}

/*
 *  Silly little functions to help parsing our argument string
 */
static int
count_tokens(const char *str, char token, char token_end)
{
    int count = 0;

    for (; *str; str++) {
        if (*str == token) {
            count++;
        }
        if (*str == token_end) {
            break;
        }
    }
    return count;
}

static const char *
strip(const char *str)
{
    for (; *str && isspace(*str); str++) {
    }
    return str;
}

static const char *
find_blank(const char *str)
{
    for (; *str && !isspace(*str); str++) {
    }
    return str;
}


/*
 *  We really want to use some existing argument parsing library here. That
 *  would give us a consistent look */
static VCardEmulOptions options;
#define READER_STEP 4

/* Expects "args" to be at the beginning of a token (ie right after the ','
 * ending the previous token), and puts the next token start in "token",
 * and its length in "token_length". "token" will not be nul-terminated.
 * After calling the macro, "args" will be advanced to the beginning of
 * the next token.
 */
#define NEXT_TOKEN(token) \
            (token) = args; \
            args = strpbrk(args, ",)"); \
            if (args == NULL || *args == 0 || *args == ')') { \
                fprintf(stderr, "Error: invalid soft specification.\n"); \
                goto fail; \
            } \
            (token##_length) = args - (token); \
            args = strip(args+1);

VCardEmulOptions *
vcard_emul_options(const char *args)
{
    int i, j, reader_count = 0;
    VCardEmulOptions *opts;

    /* Allow the future use of allocating the options structure on the fly */
    memcpy(&options, &default_options, sizeof(options));
    opts = &options;

    do {
        args = strip(args); /* strip off the leading spaces */
        if (*args == ',') {
            args++;
            continue;
        }
        /* soft=(slot_name,virt_name,emul_type,emul_flags,cert_1, (no eol)
         *       cert_2,cert_3...) */
        if (strncmp(args, "soft=", 5) == 0) {
            const char *name;
            size_t name_length;
            const char *vname;
            size_t vname_length;
            const char *type_params;
            size_t type_params_length;
            char type_str[100];
            VCardEmulType type;
            int count;
            VirtualReaderOptions *vreaderOpt;

            args = strip(args + 5);
            if (*args != '(') {
                fprintf(stderr, "Error: invalid soft specification.\n");
                goto fail;
            }
            args = strip(args+1);

            NEXT_TOKEN(name)
            NEXT_TOKEN(vname)
            NEXT_TOKEN(type_params)
            type_params_length = MIN(type_params_length, sizeof(type_str)-1);
            memcpy(type_str, type_params, type_params_length);
            type_str[type_params_length] = '\0';
            type = vcard_emul_type_from_string(type_str);
            if (type == VCARD_EMUL_NONE) {
                fprintf(stderr, "Error: invalid smartcard type '%s'.\n",
                        type_str);
                goto fail;
            }

            NEXT_TOKEN(type_params)

            if (*args == 0) {
                fprintf(stderr, "Error: missing cert specification.\n");
                goto fail;
            }

            if (opts->vreader_count >= reader_count) {
                reader_count += READER_STEP;
                opts->vreader = g_renew(VirtualReaderOptions, opts->vreader,
                                        reader_count);
            }
            vreaderOpt = &opts->vreader[opts->vreader_count];
            vreaderOpt->name = g_strndup(name, name_length);
            vreaderOpt->vname = g_strndup(vname, vname_length);
            vreaderOpt->card_type = type;
            vreaderOpt->type_params =
                g_strndup(type_params, type_params_length);
            count = count_tokens(args, ',', ')') + 1;
            vreaderOpt->cert_count = count;
            vreaderOpt->cert_name = g_new(char *, count);
            for (i = 0; i < count; i++) {
                const char *cert = args;
                args = strpbrk(args, ",)");
                vreaderOpt->cert_name[i] = g_strndup(cert, args - cert);
                args = strip(args+1);
            }
            if (*args == ')') {
                args++;
            }
            opts->vreader_count++;
        /* use_hw= */
        } else if (strncmp(args, "use_hw=", 7) == 0) {
            args = strip(args+7);
            if (*args == '0' || *args == 'N' || *args == 'n' || *args == 'F') {
                opts->use_hw = USE_HW_NO;
            } else if (strncmp(args, "removable", 9) == 0) {
                opts->use_hw = USE_HW_REMOVABLE;
            } else {
                opts->use_hw = USE_HW_YES;
            }
            args = find_blank(args);
        /* hw_type= */
        } else if (strncmp(args, "hw_type=", 8) == 0) {
            args = strip(args+8);
            opts->hw_card_type = vcard_emul_type_from_string(args);
            if (opts->hw_card_type == VCARD_EMUL_NONE) {
                fprintf(stderr, "Error: invalid smartcard type '%s'.\n",
                        args);
                goto fail;
            }
            args = find_blank(args);
        /* hw_params= */
        } else if (strncmp(args, "hw_params=", 10) == 0) {
            const char *params;

            if (opts->hw_type_params != NULL) {
                fprintf(stderr, "Error: redefinition of hw_params= is not allowed.\n");
                goto fail;
            }
            args = strip(args+10);
            params = args;
            args = find_blank(args);
            opts->hw_type_params = g_strndup(params, args-params);
        /* db="/data/base/path" */
        } else if (strncmp(args, "db=", 3) == 0) {
            const char *db;

            if (opts->nss_db != NULL) {
                fprintf(stderr, "Error: redefinition of db= is not allowed.\n");
                goto fail;
            }
            args = strip(args+3);
            if (*args != '"') {
                fprintf(stderr, "Error: you must quote the file path.\n");
                goto fail;
            }
            args++;
            db = args;
            args = strpbrk(args, "\"\n");
            if (args == NULL) {
                fprintf(stderr, "Error: invalid db argument.\n");
                goto fail;
            }
            opts->nss_db = g_strndup(db, args-db);
            if (*args != 0) {
                args++;
            }
        } else if (strncmp(args, "nssemul", 7) == 0) {
            opts->hw_card_type = VCARD_EMUL_CAC;
            opts->use_hw = USE_HW_YES;
            args = find_blank(args + 7);
#if defined(ENABLE_PCSC)
        } else if (strncmp(args, "passthru", 8) == 0) {
            opts->hw_card_type = VCARD_EMUL_PASSTHRU;
            opts->use_hw = USE_HW_YES;
            args = find_blank(args + 8);
#endif
        } else {
            fprintf(stderr, "Error: Unknown smartcard specification.\n");
            goto fail;
        }
    } while (*args != 0);

    return opts;

fail:
    /* Clean up what was allocated above on failure */
    for (i = 0; i < opts->vreader_count; i++) {
        g_free(opts->vreader[i].name);
        g_free(opts->vreader[i].vname);
        g_free(opts->vreader[i].type_params);
        for (j = 0; j < opts->vreader[i].cert_count; j++) {
            g_free(opts->vreader[i].cert_name[j]);
        }
        g_free(opts->vreader[i].cert_name);
    }
    g_free(opts->vreader);
    g_free(opts->hw_type_params);
    g_free(opts->nss_db);
    return NULL;
}

unsigned char *
vcard_emul_read_object(VCard *card, const char *label,
    unsigned int *ret_len)
{
    PK11SlotInfo *slot;
    PK11GenericObject *obj, *firstObj, *myObj = NULL;
    SECItem result;
    SECStatus r;
    unsigned char *ret;

    slot = vcard_emul_card_get_slot(card);

    firstObj = PK11_FindGenericObjects(slot, CKO_DATA);
    g_debug("%s: Search for generic objects: got %p", __func__, firstObj);
    for (obj = firstObj; obj; obj = PK11_GetNextGenericObject(obj)) {
        int found = 0;
        r = PK11_ReadRawAttribute(PK11_TypeGeneric, obj,
            CKA_LABEL, &result);
        if (r != SECSuccess) {
            PK11_DestroyGenericObjects(firstObj);
            return NULL;
        }

        if (strlen(label) == result.len
            && memcmp(label, result.data, result.len) == 0)
            found = 1;

        PORT_Free(result.data);
        result.data = NULL;

        if (found) {
            PK11_UnlinkGenericObject(obj);
            myObj = obj;
            break;
        }
    }
    PK11_DestroyGenericObjects(firstObj);

    if (!myObj)
        return NULL;

    r = PK11_ReadRawAttribute(PK11_TypeGeneric, myObj,
        CKA_VALUE, &result);
    PK11_DestroyGenericObject(myObj);
    if (r != SECSuccess)
        return NULL;

    *ret_len = result.len;
    ret = g_memdup2(result.data, result.len);
    PORT_Free(result.data);
    return ret;
}

void
vcard_emul_usage(void)
{
   fprintf(stderr,
"emul args: comma separated list of the following arguments\n"
" db={nss_database}               (default sql:/etc/pki/nssdb)\n"
" use_hw=[yes|no|removable]       (default yes)\n"
" hw_type={card_type_to_emulate}  (default CAC)\n"
" hw_params={param_for_card}      (default \"\")\n"
" nssemul                         (alias for use_hw=yes, hw_type=CAC)\n"
#if defined(ENABLE_PCSC)
" passthru                        (alias for use_hw=yes, hw_type=PASSTHRU)\n"
#endif
" soft=({slot_name},{vreader_name},{card_type_to_emulate},{params_for_card},\n"
"       {cert1},{cert2},{cert3}    (default none)\n"
"\n"
"  {nss_database}          The location of the NSS cert & key database\n"
"  {card_type_to_emulate}  What card interface to present to the guest\n"
"  {param_for_card}        Card interface specific parameters\n"
"  {slot_name}             NSS slot that contains the certs\n"
"  {vreader_name}          Virtual reader name to present to the guest\n"
"  {certN}                 Nickname of the certificate n on the virtual card\n"
"\n"
"These parameters come as a single string separated by blanks or newlines."
"\n"
"Unless use_hw is set to no, all tokens that look like removable hardware\n"
"tokens will be presented to the guest using the emulator specified by\n"
"hw_type, and parameters of hw_params. If use_hw is set to 'removable', "
"present any removable token.\n"
"\n"
"If more one or more soft= parameters are specified, these readers will be\n"
"presented to the guest\n"
#if defined(ENABLE_PCSC)
"\n"
"If a hw_type of PASSTHRU is given, a connection will be made to the hardware\n"
"using libpcscslite.  Note that in that case, no soft cards are permitted.\n"
#endif
);
}
/* vim: set ts=4 sw=4 tw=0 noet expandtab: */
