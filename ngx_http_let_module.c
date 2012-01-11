#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <stdlib.h>
#include <time.h>
#include "let.h"

#include <openssl/md4.h>
#include <openssl/md5.h>
#include <openssl/sha.h>
#include <openssl/ripemd.h>

static char* ngx_http_let_let(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

/* Module commands */
static ngx_command_t ngx_http_let_commands[] = {

	{	ngx_string("let"),
		NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
		ngx_http_let_let,
		NGX_HTTP_LOC_CONF_OFFSET,
		0,
		NULL },

	ngx_null_command
};

/* Module context */
static ngx_http_module_t ngx_http_let_module_ctx = {

    NULL,                              /* preconfiguration */
    NULL,                              /* postconfiguration */
    NULL,                              /* create main configuration */
    NULL,                              /* init main configuration */
    NULL,                              /* create server configuration */
    NULL,                              /* merge server configuration */
    NULL,                              /* create location configuration */
    NULL                               /* merge location configuration */
};

/* Module */
ngx_module_t ngx_http_let_module = {

	NGX_MODULE_V1,
	&ngx_http_let_module_ctx,          /* module context */
	ngx_http_let_commands,             /* module directives */
	NGX_HTTP_MODULE,                   /* module type */
	NULL,                              /* init master */
	NULL,                              /* init module */
	NULL,                              /* init process */
	NULL,                              /* init thread */
	NULL,                              /* exit thread */
	NULL,                              /* exit process */
	NULL,                              /* exit master */
	NGX_MODULE_V1_PADDING
};

static ngx_int_t ngx_let_toi(ngx_str_t* s) 
{
	return (s->len > 2 && s->data[0] == '0' && s->data[1] == 'x')

		? ngx_hextoi(s->data + 2, s->len - 2)

		: ngx_atoi(s->data, s->len);
}

/* Function engine */
static ngx_int_t ngx_let_func_rand(ngx_http_request_t *r, ngx_str_t *ret)
{
	ret->len = 32;
	ret->data = ngx_palloc(r->pool, ret->len);

	ret->len = ngx_snprintf(ret->data, ret->len, "%d", rand()) - ret->data;

	return NGX_OK;
}

#define NGX_LET_HASHFUNC(fun, name, hashlen) \
static ngx_int_t ngx_let_func_##name(ngx_http_request_t *r, \
		ngx_str_t *arg, ngx_str_t *ret) \
{ \
	u_char md[hashlen]; \
	unsigned n; \
	u_char *s; \
	static u_char hex[] = "0123456789abcdef"; \
\
	ret->len = sizeof(md) * 2; \
	ret->data = ngx_palloc(r->pool, ret->len); \
\
	fun(arg->data, arg->len, md); \
\
	for(n = 0, s = ret->data; n < sizeof(md); ++n) { \
		*s++ = hex[(md[n] >> 4) & 0x0f]; \
		*s++ = hex[md[n] & 0x0f]; \
	} \
\
	return NGX_OK; \
}

NGX_LET_HASHFUNC(MD4, md4, 16)
NGX_LET_HASHFUNC(MD5, md5, 16)

NGX_LET_HASHFUNC(SHA1,   sha1,   20)
NGX_LET_HASHFUNC(SHA224, sha224, 28)
NGX_LET_HASHFUNC(SHA256, sha256, 32)
NGX_LET_HASHFUNC(SHA384, sha384, 48)
NGX_LET_HASHFUNC(SHA512, sha512, 64)

NGX_LET_HASHFUNC(RIPEMD160, ripemd160, 20)

static ngx_int_t ngx_let_func_length(ngx_http_request_t *r, 
		ngx_str_t *str, ngx_str_t *ret)
{
	ret->len = 32;
	ret->data = ngx_palloc(r->pool, ret->len);

	ret->len = ngx_snprintf(ret->data, ret->len, "%d", str->len) - ret->data;

	return NGX_OK;
}

#define NGX_LET_ICMPFUNC(name, op) \
static ngx_int_t ngx_let_func_##name(ngx_http_request_t *r, \
		ngx_str_t *a1, ngx_str_t *a2, ngx_str_t *ret) \
{ \
	ngx_int_t v1, v2; \
	ret->len = 32; \
	ret->data = ngx_palloc(r->pool, ret->len); \
\
	v1 = ngx_atoi(a1->data, a1->len); \
	v2 = ngx_atoi(a2->data, a2->len); \
\
	ret->len = ngx_snprintf(ret->data, ret->len, "%d", \
		v1 op v2 ? v1 : v2) - ret->data; \
\
	return NGX_OK; \
}

NGX_LET_ICMPFUNC(min, <)
NGX_LET_ICMPFUNC(max, >)

static ngx_int_t ngx_let_func_substr(ngx_http_request_t *r, 
		ngx_str_t *str, ngx_str_t *offset,
		ngx_str_t *length, ngx_str_t *ret)
{
	ngx_int_t offs, len;

	*ret = *str;

	offs = ngx_atoi(offset->data, offset->len);
	len = ngx_atoi(length->data, length->len);

	if (offs >= (ngx_int_t)ret->len) {
		ret->len = 0;
		return NGX_OK;
	}

	ret->data += offs;

	if (!len || offs + len >= (ngx_int_t)ret->len)
		ret->len -= offs;
	else
		ret->len = len;

	return NGX_OK;
}

/* Call function by name & return result */
static ngx_int_t ngx_let_call_fun(ngx_http_request_t *r,
		ngx_str_t *name, ngx_array_t *args, ngx_str_t *value)
{
	ngx_str_t *sargs = args->elts;

	/* TODO: implement hashtable for faster lookup */

#define IF_FUNC(nm, nargs) \
	if (sizeof(#nm) - 1 == name->len \
			&& !ngx_strncmp(#nm, name->data, name->len)) { \
		if (nargs != args->nelts) { \
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, \
				"let function '%*s' expects %d arguments, %d provided", \
					name->len, name->data, nargs, args->nelts); \
			return NGX_ERROR; \
		}

#define CALL_FUNC_0(nm) \
	IF_FUNC(nm, 0) \
		return ngx_let_func_##nm(r, value); \
	}

#define CALL_FUNC_1(nm) \
	IF_FUNC(nm, 1) \
		return ngx_let_func_##nm(r, sargs, value); \
	}

#define CALL_FUNC_2(nm) \
	IF_FUNC(nm, 2) \
		return ngx_let_func_##nm(r, sargs, sargs + 1, value); \
	}

#define CALL_FUNC_3(nm) \
	IF_FUNC(nm, 3) \
		return ngx_let_func_##nm(r, sargs, sargs + 1, sargs + 2, value); \
	}
	
	CALL_FUNC_0(rand);

	/* cryptographic hashes */
	CALL_FUNC_1(md4);
	CALL_FUNC_1(md5);

	CALL_FUNC_1(sha1);
	CALL_FUNC_1(sha224);
	CALL_FUNC_1(sha256);
	CALL_FUNC_1(sha384);
	CALL_FUNC_1(sha512);

	CALL_FUNC_1(ripemd160);

	/* string operations */
	CALL_FUNC_1(length);
	CALL_FUNC_3(substr);

	/* integer operations */
	CALL_FUNC_2(max);
	CALL_FUNC_2(min);

	ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
				"let undefined function '%*s'", name->len, name->data);

	return NGX_ERROR;
}

/* Processes positive integers only */
static ngx_int_t ngx_let_apply_binary_integer_op(ngx_http_request_t *r, int op, 
		ngx_array_t* args, ngx_str_t* value)
{
	ngx_str_t* str;
	int orig_left, left, right;
	unsigned sz;

	if (args->nelts != 2) {
		ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
				"let not enough argument for binary operation");
		return NGX_ERROR;
	}
	
	str = args->elts;

	left = ngx_let_toi(str);
	if (left != NGX_ERROR) {
		orig_left = left;
		++str;
		right = ngx_let_toi(str);
	}
	
	if (left == NGX_ERROR || right == NGX_ERROR) {
		ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
				"let error parsing argument '%*s'", str->len, str->data);
		return NGX_ERROR;
	}
	
	switch(op) {
		
		case '+':
			left += right;
			break;
			
		case '-':
			left -= right;
			break;

		case '*':
			left *= right;
			break;

		case '/':
			left /= right;
			break;

		case '%':
			left %= right;
			break;

		case '&':
			left &= right;
			break;

		case '|':
			left |= right;
			break;

		default:
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
					"let unexpected operation '%c'", op);
			return NGX_ERROR;
	}
	
	value->len = 64; /*TODO: better size? */
	value->data = ngx_palloc(r->pool, value->len);
	
	sz = snprintf((char*)value->data, value->len, "%d", left);

	if (sz < value->len)
		value->len = sz;
	
	ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"let applying binary operation %d '%c' %d: %d", orig_left, op, right, left);

	return NGX_OK;
}

static ngx_int_t ngx_let_get_node_value(ngx_http_request_t* r, ngx_let_node_t* node,
		ngx_str_t* value)
{
	ngx_http_variable_value_t* vv;
	ngx_array_t args;
	ngx_let_node_t** anode;
	ngx_str_t* astr;
	ngx_uint_t n;
	ngx_int_t ret;
	u_char* s;
	int *cap;
	ngx_int_t ncap;

	if (node == NULL) {
		ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
				"let NULL node");
		return NGX_ERROR;
	}

	switch(node->type) {
		
		case NGX_LTYPE_VARIABLE:
			
			vv = ngx_http_get_indexed_variable(r, node->index);
				
			if (vv == NULL || vv->not_found) {
				ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let variable %d not found", node->index);
					
				return NGX_ERROR;
			}

			value->data = vv->data;
			value->len = vv->len;
			
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let getting variable %d: '%*s'", node->index, value->len, value->data);

			break;

		case NGX_LTYPE_CAPTURE:

			if (node->index >= (ngx_int_t)r->ncaptures)
				return NGX_ERROR;

			cap = r->captures;

			ncap = node->index * 2;

			value->data = r->captures_data + cap[ncap];
			value->len = cap[ncap + 1] - cap[ncap];

			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let getting capture %d: '%*s'", node->index, value->len, value->data);

			break;
			
		case NGX_LTYPE_LITERAL:
			
			*value = node->name;
			
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let getting literal: '%*s'", value->len, value->data);
			
			break;

		case NGX_LTYPE_FUNCTION:

			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let calling function '%*s'; argc: %d", 
						node->name.len, node->name.data, node->args.nelts);
			
			/* parse arguments */
			ngx_array_init(&args, r->pool, node->args.nelts, sizeof(ngx_str_t));

			astr = ngx_array_push_n(&args, node->args.nelts);
			anode = node->args.elts;
		
			for(n = 0; n < node->args.nelts; ++n) {
				
				ret = ngx_let_get_node_value(r, *anode++, astr++);
				if (ret != NGX_OK)
					return ret;
			}

			ret = ngx_let_call_fun(r, &node->name, &args, value);

			if (ret != NGX_OK)
				return ret;

			break;
			
		case NGX_LTYPE_OPERATION:
			
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let applying operation '%c'; argc: %d", node->index, node->args.nelts);
			
			/* parse arguments */
			ngx_array_init(&args, r->pool, node->args.nelts, sizeof(ngx_str_t));

			astr = ngx_array_push_n(&args, node->args.nelts);
			anode = node->args.elts;
		
			for(n = 0; n < node->args.nelts; ++n) {
				
				ret = ngx_let_get_node_value(r, *anode++, astr++);
				if (ret != NGX_OK)
					return ret;
			}
			
			if (strchr("+-*/%&|", node->index)) {
				
				/* binary integer operation */

				ret = ngx_let_apply_binary_integer_op(r, node->index, &args, value);
				if (ret != NGX_OK)
					return ret;
				
			} else if (node->index == '.') {
				
				/* string concatenation */

				value->len = 0;
				astr = args.elts;
				
				for(n = 0; n < args.nelts; ++n, ++astr)
					value->len += astr->len;

				value->data = ngx_palloc(r->pool, value->len);

				astr = args.elts;
				s = value->data;
				for(n = 0, s = value->data; n < args.nelts; ++n, s += astr++->len)
					memcpy(s, astr->data, astr->len);
				
				ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let %d strings concatenated '%*s'", args.nelts, value->len, value->data);
			}

			break;
	}

	return NGX_OK;
}

static ngx_int_t ngx_http_let_variable(ngx_http_request_t *r,
		    ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_let_node_t* node = (ngx_let_node_t*)data;
	ngx_str_t value;
	ngx_int_t ret;

	ret = ngx_let_get_node_value(r, node, &value);

	if (ret == NGX_OK) {

		v->len = value.len;
		v->data = value.data;
		v->valid = 1;
		v->no_cacheable = 0;
		v->not_found = 0;
			
		ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "let variable accessed");
	}

	return ret;
}

static char* ngx_http_let_let(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_str_t *value;
	ngx_http_variable_t *v;

	srand(time(0));
	
	value = cf->args->elts;
	
	ngx_log_debug(NGX_LOG_INFO, cf->log, 0, "let command handler");

	if (value[1].data[0] != '$')
		return "needs variable as the first argument";

	value[1].data++;
	value[1].len--;

	v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);

	v->get_handler = ngx_http_let_variable;
	v->data = (uintptr_t)ngx_parse_let_expr(cf);
	
	return NGX_CONF_OK;
}

