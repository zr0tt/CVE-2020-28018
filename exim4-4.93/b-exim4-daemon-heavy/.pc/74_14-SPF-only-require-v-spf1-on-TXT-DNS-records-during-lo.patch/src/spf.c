/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* SPF support.
   Copyright (c) Tom Kistner <tom@duncanthrax.net> 2004 - 2014
   License: GPL
   Copyright (c) The Exim Maintainers 2015 - 2019
*/

/* Code for calling spf checks via libspf-alt. Called from acl.c. */

#include "exim.h"
#ifdef SUPPORT_SPF

/* must be kept in numeric order */
static spf_result_id spf_result_id_list[] = {
  /* name		value */
  { US"invalid",	0},
  { US"neutral",	1 },
  { US"pass",		2 },
  { US"fail",		3 },
  { US"softfail",	4 },
  { US"none",		5 },
  { US"temperror",	6 }, /* RFC 4408 defined */
  { US"permerror",	7 }  /* RFC 4408 defined */
};

SPF_server_t    *spf_server = NULL;
SPF_request_t   *spf_request = NULL;
SPF_response_t  *spf_response = NULL;
SPF_response_t  *spf_response_2mx = NULL;

SPF_dns_rr_t  * spf_nxdomain = NULL;



static SPF_dns_rr_t *
SPF_dns_exim_lookup(SPF_dns_server_t *spf_dns_server,
const char *domain, ns_type rr_type, int should_cache)
{
dns_answer * dnsa = store_get_dns_answer();
dns_scan dnss;
SPF_dns_rr_t * spfrr;

DEBUG(D_receive) debug_printf("SPF_dns_exim_lookup\n");

if (dns_lookup(dnsa, US domain, rr_type, NULL) == DNS_SUCCEED)
  for (dns_record * rr = dns_next_rr(dnsa, &dnss, RESET_ANSWERS); rr;
       rr = dns_next_rr(dnsa, &dnss, RESET_NEXT))
    if (  rr->type == rr_type
       && Ustrncmp(rr->data+1, "v=spf1", 6) == 0)
      {
      gstring * g = NULL;
      uschar chunk_len;
      uschar * s;
      SPF_dns_rr_t srr = {
	.domain = CS rr->name,			/* query information */
	.domain_buf_len = DNS_MAXNAME,
	.rr_type = rr->type,

	.num_rr = 1,				/* answer information */
	.rr = NULL,
	.rr_buf_len = 0,
	.rr_buf_num = 0,
	.ttl = rr->ttl,
	.utc_ttl = 0,
	.herrno = NETDB_SUCCESS,

	.hook = NULL,				/* misc information */
	.source = spf_dns_server
      };

      for (int off = 0; off < rr->size; off += chunk_len)
	{
	chunk_len = (rr->data)[off++];
	g = string_catn(g, US ((rr->data)+off), chunk_len);
	}
      if (!g)
	{
	HDEBUG(D_host_lookup) debug_printf("IP address lookup yielded an "
	  "empty name: treated as non-existent host name\n");
	continue;
	}
      gstring_release_unused(g);
      s = string_copy_malloc(string_from_gstring(g));
      srr.rr = (void *) &s;

      /* spfrr->rr must have been malloc()d for this */
      SPF_dns_rr_dup(&spfrr, &srr);

      return spfrr;
      }

SPF_dns_rr_dup(&spfrr, spf_nxdomain);
return spfrr;
}



SPF_dns_server_t *
SPF_dns_exim_new(int debug)
{
SPF_dns_server_t *spf_dns_server;

DEBUG(D_receive) debug_printf("SPF_dns_exim_new\n");

if (!(spf_dns_server = malloc(sizeof(SPF_dns_server_t))))
  return NULL;
memset(spf_dns_server, 0, sizeof(SPF_dns_server_t));

spf_dns_server->destroy      = NULL;
spf_dns_server->lookup       = SPF_dns_exim_lookup;
spf_dns_server->get_spf      = NULL;
spf_dns_server->get_exp      = NULL;
spf_dns_server->add_cache    = NULL;
spf_dns_server->layer_below  = NULL;
spf_dns_server->name         = "exim";
spf_dns_server->debug        = debug;

/* XXX This might have to return NO_DATA sometimes. */

spf_nxdomain = SPF_dns_rr_new_init(spf_dns_server,
  "", ns_t_any, 24 * 60 * 60, HOST_NOT_FOUND);
if (!spf_nxdomain)
  {
  free(spf_dns_server);
  return NULL;
  }

return spf_dns_server;
}




/* Construct the SPF library stack.
   Return: Boolean success.
*/

BOOL
spf_init(void)
{
SPF_dns_server_t * dc;
int debug = 0;

DEBUG(D_receive) debug = 1;

/* We insert our own DNS access layer rather than letting the spf library
do it, so that our dns access path is used for debug tracing and for the
testsuite. */

if (!(dc = SPF_dns_exim_new(debug)))
  {
  DEBUG(D_receive) debug_printf("spf: SPF_dns_exim_new() failed\n");
  return FALSE;
  }
if (!(dc = SPF_dns_cache_new(dc, NULL, debug, 8)))
  {
  DEBUG(D_receive) debug_printf("spf: SPF_dns_cache_new() failed\n");
  return FALSE;
  }
if (!(spf_server = SPF_server_new_dns(dc, debug)))
  {
  DEBUG(D_receive) debug_printf("spf: SPF_server_new() failed.\n");
  return FALSE;
  }
  /* Quick hack to override the outdated explanation URL.
  See https://www.mail-archive.com/mailop@mailop.org/msg08019.html */
  SPF_server_set_explanation(spf_server, "Please%_see%_http://www.open-spf.org/Why?id=%{S}&ip=%{C}&receiver=%{R}", &spf_response);
  if (SPF_response_errcode(spf_response) != SPF_E_SUCCESS)
    log_write(0, LOG_MAIN|LOG_PANIC_DIE, "%s", SPF_strerror(SPF_response_errcode(spf_response)));

return TRUE;
}


/* Set up a context that can be re-used for several
   messages on the same SMTP connection (that come from the
   same host with the same HELO string).

Return: Boolean success
*/

BOOL
spf_conn_init(uschar * spf_helo_domain, uschar * spf_remote_addr)
{
DEBUG(D_receive)
  debug_printf("spf_conn_init: %s %s\n", spf_helo_domain, spf_remote_addr);

if (!spf_server && !spf_init()) return FALSE;

if (SPF_server_set_rec_dom(spf_server, CS primary_hostname))
  {
  DEBUG(D_receive) debug_printf("spf: SPF_server_set_rec_dom(\"%s\") failed.\n",
    primary_hostname);
  spf_server = NULL;
  return FALSE;
  }

spf_request = SPF_request_new(spf_server);

if (  SPF_request_set_ipv4_str(spf_request, CS spf_remote_addr)
   && SPF_request_set_ipv6_str(spf_request, CS spf_remote_addr)
   )
  {
  DEBUG(D_receive)
    debug_printf("spf: SPF_request_set_ipv4_str() and "
      "SPF_request_set_ipv6_str() failed [%s]\n", spf_remote_addr);
  spf_server = NULL;
  spf_request = NULL;
  return FALSE;
  }

if (SPF_request_set_helo_dom(spf_request, CS spf_helo_domain))
  {
  DEBUG(D_receive) debug_printf("spf: SPF_set_helo_dom(\"%s\") failed.\n",
    spf_helo_domain);
  spf_server = NULL;
  spf_request = NULL;
  return FALSE;
  }

return TRUE;
}


/* spf_process adds the envelope sender address to the existing
   context (if any), retrieves the result, sets up expansion
   strings and evaluates the condition outcome.

Return: OK/FAIL  */

int
spf_process(const uschar **listptr, uschar *spf_envelope_sender, int action)
{
int sep = 0;
const uschar *list = *listptr;
uschar *spf_result_id;
int rc = SPF_RESULT_PERMERROR;

DEBUG(D_receive) debug_printf("spf_process\n");

if (!(spf_server && spf_request))
  /* no global context, assume temp error and skip to evaluation */
  rc = SPF_RESULT_PERMERROR;

else if (SPF_request_set_env_from(spf_request, CS spf_envelope_sender))
  /* Invalid sender address. This should be a real rare occurrence */
  rc = SPF_RESULT_PERMERROR;

else
  {
  /* get SPF result */
  if (action == SPF_PROCESS_FALLBACK)
    {
    SPF_request_query_fallback(spf_request, &spf_response, CS spf_guess);
    spf_result_guessed = TRUE;
    }
  else
    SPF_request_query_mailfrom(spf_request, &spf_response);

  /* set up expansion items */
  spf_header_comment     = US SPF_response_get_header_comment(spf_response);
  spf_received           = US SPF_response_get_received_spf(spf_response);
  spf_result             = US SPF_strresult(SPF_response_result(spf_response));
  spf_smtp_comment       = US SPF_response_get_smtp_comment(spf_response);

  rc = SPF_response_result(spf_response);
  }

/* We got a result. Now see if we should return OK or FAIL for it */
DEBUG(D_acl) debug_printf("SPF result is %s (%d)\n", SPF_strresult(rc), rc);

if (action == SPF_PROCESS_GUESS && (!strcmp (SPF_strresult(rc), "none")))
  return spf_process(listptr, spf_envelope_sender, SPF_PROCESS_FALLBACK);

while ((spf_result_id = string_nextinlist(&list, &sep, NULL, 0)))
  {
  BOOL negate, result;

  if ((negate = spf_result_id[0] == '!'))
    spf_result_id++;

  result = Ustrcmp(spf_result_id, spf_result_id_list[rc].name) == 0;
  if (negate != result) return OK;
  }

/* no match */
return FAIL;
}



gstring *
authres_spf(gstring * g)
{
uschar * s;
if (!spf_result) return g;

g = string_append(g, 2, US";\n\tspf=", spf_result);
if (spf_result_guessed)
  g = string_cat(g, US" (best guess record for domain)");

s = expand_string(US"$sender_address_domain");
return s && *s
  ? string_append(g, 2, US" smtp.mailfrom=", s)
  : string_cat(g, US" smtp.mailfrom=<>");
}


#endif
