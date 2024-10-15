#ifndef KRB5_UTIL_H
#define KRB5_UTIL_H

#include <krb5.h>

#ifdef HAVE_LIBTIRPC
#include <rpc/auth_gss.h>
#else
#include "gss_oids.h"
#endif


int gssd_setup_krb5_user_gss_ccache(uid_t uid, char *servername,
				     char *dirname);
int  gssd_get_krb5_machine_cred_list(char ***list);
void gssd_free_krb5_machine_cred_list(char **list);
void gssd_destroy_krb5_principals(int destroy_machine_creds);
int  gssd_refresh_krb5_machine_credential(char *hostname,
					  char *service, char *srchost,
					  int force_renew);
char *gssd_k5_err_msg(krb5_context context, krb5_error_code code);
void gssd_k5_get_default_realm(char **def_realm);

int gssd_acquire_user_cred(gss_cred_id_t *gss_cred);
int gssd_k5_remove_bad_service_cred(char *srvname);

#ifdef HAVE_SET_ALLOWABLE_ENCTYPES
extern int limit_to_legacy_enctypes;
int limit_krb5_enctypes(struct rpc_gss_sec *sec);
#endif

/*
 * Hide away some of the MIT vs. Heimdal differences
 * here with macros...
 */

#ifdef HAVE_KRB5
#define k5_free_unparsed_name(ctx, name)	krb5_free_unparsed_name((ctx), (name))
#define k5_free_default_realm(ctx, realm)	krb5_free_default_realm((ctx), (realm))
#define k5_free_kt_entry(ctx, kte)		krb5_free_keytab_entry_contents((ctx),(kte))
#else	/* Heimdal */
#define k5_free_unparsed_name(ctx, name)	free(name)
#define k5_free_default_realm(ctx, realm)	free(realm)
#define k5_free_kt_entry(ctx, kte)		krb5_kt_free_entry((ctx),(kte))
#endif

#endif /* KRB5_UTIL_H */
