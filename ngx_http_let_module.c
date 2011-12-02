#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include "let.h"

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

/* Processes positive integers only */
static ngx_int_t ngx_let_apply_binary_integer_op(ngx_http_request_t *r, int op, 
		ngx_array_t* args, ngx_str_t* value)
{
	ngx_str_t* str;
	int orig_left, left, right;
	unsigned sz;

	if (args->nelts != 2) {
		ngx_log_debug(NGX_LOG_INFO, r->connection->log, 0, 
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
		ngx_log_debug(NGX_LOG_INFO, r->connection->log, 0, 
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
			
		case NGX_LTYPE_LITERAL:
			
			*value = node->name;
			
			ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let getting literal: '%*s'", value->len, value->data);
			
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
			
			if (strchr("+-*/%", node->index)) {
				
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
	
	value = cf->args->elts;
	
	ngx_log_debug(NGX_LOG_INFO, cf->log, 0, "let command handler");

	if (value[1].data[0] != '$')
		return NGX_CONF_ERROR;

	value[1].data++;
	value[1].len--;

	v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);

	v->get_handler = ngx_http_let_variable;
	v->data = (uintptr_t)ngx_parse_let_expr(cf);
	
	return NGX_CONF_OK;
}

