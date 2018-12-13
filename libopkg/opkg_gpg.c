/* vi: set expandtab sw=4 sts=4: */
/* opkg_gpg.c - the opkg package management system

    Copyright (C) 2001 University of Southern California
    Copyright (C) 2008 OpenMoko Inc
    Copyright (C) 2014 Paul Barker

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2, or (at
    your option) any later version.

    This program is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    General Public License for more details.
*/

#include "config.h"

#include <gpgme.h>
#include <stdlib.h>

#include "opkg_conf.h"
#include "opkg_message.h"
#include "opkg_gpg.h"
#include "sprintf_alloc.h"
#include "file_util.h"

static int gpgme_init()
{
    int ret = -1;
    gpgme_error_t err;
    gpgme_engine_info_t info;

    gpgme_check_version(NULL);

    err = gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP);
    if (err) {
        opkg_msg(ERROR, "GPGME Engine Check Failed: %s\n", gpg_strerror(err));
        goto err;
    }

    err = gpgme_set_engine_info(GPGME_PROTOCOL_OpenPGP, NULL, opkg_config->gpg_dir);
    if (err) {
        opkg_msg(ERROR, "GPGME Failed to set signature directory: %s\n", gpg_strerror(err));
        goto err;
    }

    if (opkg_config->verbosity >= DEBUG2 ) {
        err = gpgme_get_engine_info(&info);
        if (err) {
            opkg_msg(ERROR, "GPGME Failed to get engine info: %s\n", gpg_strerror(err));
            goto err;
        }

        while (info){
            opkg_msg(DEBUG2, "GPGME Engine Info\n");
            opkg_msg(DEBUG2, "protocol: %s\n", gpgme_get_protocol_name(info->protocol));
            opkg_msg(DEBUG2, "home_dir: %s\n", info->home_dir);
            opkg_msg(DEBUG2, "filename: %s\n", info->file_name);
            info = info->next;
        }
    }

    ret = 0;
err:
    return ret;
}

/* Find all keys given the provided fingerprints given the new context.
   This is needed as GPGME's old context has the newly added key in the
   public keyring. By loading a new context the new context SHOULD not
   have loaded the public keyring, only the trusted.gpg/trustdb.gpg */
static int find_trusted_gpg_fingerprints(const char *fpr)
{
    int ret = -1;
    int has_key, has_ctx = 0;
    gpgme_error_t err;
    gpgme_ctx_t ctx;
    gpgme_key_t key;

    err = gpgme_new(&ctx);
    if (err) {
        opkg_msg(ERROR, "Unable to create gpgme context: %s\n",
            gpg_strerror(err));
        goto out_err;
    }
    has_ctx = 1;

    err = gpgme_get_key(ctx, fpr, &key, 0);
    if (err) {
        opkg_msg(DEBUG, "Unable to find public key: %s\n", gpg_strerror(err));
        goto out_err;
    }
    has_key = 1;

    /* The key has been marked as revoked */
    if (key->revoked) {
        opkg_msg(DEBUG, "Key has been revoked.\n");
        goto out_err;
    }

    /* The key has expired */
    if (key->expired) {
        opkg_msg(DEBUG, "Key has expired\n");
        goto out_err;
    }

    /* The key is marked as disabled */
    if (key->disabled) {
        opkg_msg(DEBUG, "Key has been disabled\n");
        goto out_err;
    }

    /* The key has been marked as invalid by GPGME. */
    if (key->invalid) {
        opkg_msg(DEBUG, "The key is invalid\n");
        goto out_err;
    }

    opkg_msg(DEBUG, "key->owner_name: %s\n", key->uids->name);
    opkg_msg(DEBUG, "key->owner_trust: %d\n", key->owner_trust);

    /* Always fail on GPGME_VALIDITY_NEVER as this key has been explicitly set NOT to be trusted*/
    if(key->owner_trust != GPGME_VALIDITY_NEVER){
        /* Trust level must be GPGME_VALIDITY_FULL or greater */
        if(strncmp(opkg_config->gpg_trust_level,
                    OPKG_CONF_GPG_TRUST_ONLY,
                    sizeof(OPKG_CONF_GPG_TRUST_ONLY)) == 0){
            if(key->owner_trust >= GPGME_VALIDITY_FULL){
                ret = 0;
            }
        }else if(strncmp(opkg_config->gpg_trust_level,
                    OPKG_CONF_GPG_TRUST_ANY,
                    sizeof(OPKG_CONF_GPG_TRUST_ANY)) == 0){
            ret = 0;
        }
    }
out_err:
    if(has_key)
        gpgme_key_release(key);
    if(has_ctx)
        gpgme_release(ctx);

    return ret;
}

int opkg_verify_gpg_signature(const char *file, const char *sigfile)
{
    int ret = -1;
    int trust = 0;
    gpgme_ctx_t ctx;
    int have_ctx = 0;
    gpgme_data_t sig, text, key;
    int have_sig = 0, have_text = 0, have_key = 0;
    gpgme_error_t err;
    gpgme_verify_result_t result;
    gpgme_signature_t s;
    char *trusted_path = NULL;

    if (gpgme_init()) {
        opkg_msg(ERROR, "GPGME Failed to initalize.\n");
        goto out_err;
    }

    err = gpgme_new(&ctx);
    if (err) {
        opkg_msg(ERROR, "Unable to create gpgme context: %s\n",
                 gpg_strerror(err));
        goto out_err;
    }
    have_ctx = 1;

    gpgme_engine_info_t info;
    gpgme_get_engine_info(&info);
    gpgme_ctx_set_engine_info(ctx, info->protocol, info->file_name, info->home_dir);

    /* First try standard filename */
    sprintf_alloc(&trusted_path, "%s/%s", opkg_config->gpg_dir, "pubring.kbx");
    if (!trusted_path) {
        opkg_msg(ERROR, "Out of memory!\n");
        goto out_err;
    }

    if (!file_exists(trusted_path)) {
        free(trusted_path);
        trusted_path = NULL;

        /* Fallback to old filename for compatibility */
        sprintf_alloc(&trusted_path, "%s/%s", opkg_config->gpg_dir, "trusted.gpg");
        if (!trusted_path) {
            opkg_msg(ERROR, "Out of memory!\n");
            goto out_err;
        }

        if (!file_exists(trusted_path)) {
            opkg_msg(ERROR, "Could not find pubring.kbx or trusted.gpg under %s\n",
                opkg_config->gpg_dir);
            goto out_err;
        }
    }

    err = gpgme_data_new_from_file(&key, trusted_path, 1);
    if (err) {
        opkg_msg(ERROR, "Unable to get data from file %s: %s\n", trusted_path,
                 gpg_strerror(err));
        goto out_err;
    }
    have_key = 1;

    err = gpgme_op_import(ctx, key);
    if (err) {
        opkg_msg(ERROR, "Unable to import key from file %s: %s\n", trusted_path,
                 gpg_strerror(err));
        goto out_err;
    }

    err = gpgme_data_new_from_file(&sig, sigfile, 1);
    if (err) {
        opkg_msg(ERROR, "Unable to get data from file %s: %s\n", sigfile,
                 gpg_strerror(err));
        goto out_err;
    }
    have_sig = 1;

    err = gpgme_data_new_from_file(&text, file, 1);
    if (err) {
        opkg_msg(ERROR, "Unable to get data from file %s: %s\n", file,
                 gpg_strerror(err));
        goto out_err;
    }
    have_text = 1;

    err = gpgme_op_verify(ctx, sig, text, NULL);
    if (err) {
        opkg_msg(ERROR, "Unable to verify signature: %s\n", gpg_strerror(err));
        goto out_err;
    }

    result = gpgme_op_verify_result(ctx);
    if (!result) {
        opkg_msg(ERROR, "Unable to get verification data: %s\n",
                 gpg_strerror(err));
        goto out_err;
    }

    /* see if any of the signitures matched */
    s = result->signatures;
    while (s) {
        if (s->status != GPG_ERR_NO_ERROR) {
            opkg_msg(ERROR, "Signature status returned error: %s\n", gpg_strerror(s->status));
            goto out_err;
        }
        if(find_trusted_gpg_fingerprints(s->fpr) == 0) {
            trust=1;
            break;
        }
        s = s->next;
    }

    /* Atleast one of the public keys in trusted.gpg have been found to be trustworthy */
    if(trust)
        ret=0;
    else
        opkg_msg(ERROR, "No sufficently trusted public keys found.\n");

 out_err:
    if (have_sig)
        gpgme_data_release(sig);
    if (have_text)
        gpgme_data_release(text);
    if (have_key)
        gpgme_data_release(key);
    if (trusted_path)
        free(trusted_path);
    if (have_ctx)
        gpgme_release(ctx);

    return ret;
}
