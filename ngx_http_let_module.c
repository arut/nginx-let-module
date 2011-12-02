#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <openssl/md5.h>
#include "let.h"

#define MAX_HASH_SIZE 32

static char* ngx_http_fstorage_md5(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char* ngx_http_fstorage_mod(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char* ngx_http_fstorage_let(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static void* ngx_http_fstorage_create_loc_conf(ngx_conf_t *cf);
static char* ngx_http_fstorage_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
	
/* single MD5 slice */
typedef struct {
	
    ngx_int_t index_src;     /* index of input variable */
    ngx_uint_t start;        /* start character */
    ngx_uint_t num;          /* number of md5 chars */
	
} md5_slice;

typedef struct {

	ngx_int_t index_src;     /* index of input variable */
	ngx_int_t divisor;       /* divisor */
	
} modinfo;

typedef struct {

	ngx_int_t index_src;
	u_char op;
	ngx_int_t operand;
	
} letinfo;

/* Module configuration */
typedef struct {

	ngx_array_t md5_slices;  /* array of md5_slice */

	ngx_array_t modinfos;    /* array of modinfo */

	ngx_array_t letinfos;
	
} ngx_http_fstorage_loc_conf_t;

/* Module context */
typedef struct {

	ngx_array_t md5_hashes; /* array of strings */

} ngx_http_fstorage_ctx;

/* Module commands */
static ngx_command_t ngx_http_fstorage_commands[] = {

	{	ngx_string("md5"),
		NGX_HTTP_LOC_CONF|NGX_CONF_TAKE4,
		ngx_http_fstorage_md5,
		NGX_HTTP_LOC_CONF_OFFSET,
		0,
		NULL },

	{	ngx_string("mod"),
		NGX_HTTP_LOC_CONF|NGX_CONF_TAKE3,
		ngx_http_fstorage_mod,
		NGX_HTTP_LOC_CONF_OFFSET,
		0,
		NULL },

	{	ngx_string("let"),
		NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
		ngx_http_fstorage_let,
		NGX_HTTP_LOC_CONF_OFFSET,
		0,
		NULL },

	ngx_null_command
};

/* Module context */
static ngx_http_module_t ngx_http_fstorage_module_ctx = {
	NULL,                              /* preconfiguration */
	NULL,                              /* postconfiguration */
	NULL,                              /* create main configuration */
	NULL,                              /* init main configuration */
	NULL,                              /* create server configuration */
	NULL,                              /* merge server configuration */
	ngx_http_fstorage_create_loc_conf, /* create location configuration */
	ngx_http_fstorage_merge_loc_conf   /* merge location configuration */
};

/* Module */
ngx_module_t ngx_http_fstorage_module = {
	NGX_MODULE_V1,
	&ngx_http_fstorage_module_ctx,     /* module context */
	ngx_http_fstorage_commands,        /* module directives */
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

/* Variable getter */
static ngx_int_t ngx_http_fstorage_md5_variable(ngx_http_request_t *r,
		    ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_fstorage_loc_conf_t *mcf;
	/*ngx_http_fstorage_ctx* ctx;*/
	ngx_http_variable_value_t* vv;
	md5_slice* sl;
	unsigned char md5hash[16];
	unsigned k;
	unsigned char* b;
	static char hex[] = "0123456789abcdef";

	mcf = ngx_http_get_module_loc_conf(r, ngx_http_fstorage_module);
	
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"fstorage: md5 accessing variable: %d", data);
	
	//ctx = (ngx_http_fstorage_ctx*)ngx_http_get_module_ctx(r, ngx_http_fstorage_module);

	if (data < mcf->md5_slices.nelts) {
		
		sl = ((md5_slice*)mcf->md5_slices.elts) + data;

		vv = ngx_http_get_indexed_variable(r, sl->index_src);

		if (vv == NULL || vv->not_found) {
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
					"fstorage: mod variable %d not found", sl->index_src);
			return NGX_ERROR;
		}

		MD5(vv->data, vv->len, md5hash);

		/* TODO: store MD5 */
		
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
				"fstorage: md5 got hash of %d var", sl->index_src);
		
		v->len = sl->num;
		v->data = ngx_palloc(r->pool, sl->num);
		
		k = 0;
		b = md5hash + sl->start/2;

		/* odd? */
		if (sl->start & 1)
			v->data[k++] = hex[*b++ & 0x0f];

		for(; k < sl->num; ++b, ++k) 
		{
			v->data[k] = hex[(*b >> 4) & 0x0f];
			if (++k == sl->num)
				break;
			v->data[k] = hex[*b & 0x0f];
		}

		v->valid = 1;
		v->no_cacheable = 0;
		v->not_found = 0;
		
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"fstorage: md5 variable %d ok: '%*s'", data, v->len, v->data);
		
	} else {
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"fstorage: md5 variable %d not found", data);
		
		v->not_found = 1;
	}

	return NGX_OK;
}

static ngx_int_t ngx_http_fstorage_mod_variable(ngx_http_request_t *r,
		    ngx_http_variable_value_t *v, uintptr_t data)
{
	ngx_http_fstorage_loc_conf_t *mcf;
	modinfo* mi;
	ngx_http_variable_value_t* vv;
	ngx_int_t n;
	ngx_int_t res;
	int sz;
	
	mcf = ngx_http_get_module_loc_conf(r, ngx_http_fstorage_module);
	
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"fstorage: mod accessing variable: %d", data);
	
	if (data < mcf->modinfos.nelts) {
		
		mi = (modinfo*)(mcf->modinfos.elts) + data;

		vv = ngx_http_get_indexed_variable(r, mi->index_src);
		
		if (vv == NULL || vv->not_found) {
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
					"fstorage: mod variable %d not found", mi->index_src);
			return NGX_ERROR;
		}

		n = strtoull((char*)vv->data, 0, 10);
		res = n % mi->divisor;

		v->len = vv->len;
		v->data = ngx_palloc(r->pool, v->len);

		sz = snprintf((char*)v->data, v->len, "%d", res);
		if (sz < v->len)
			v->len = sz;
		
		v->valid = 1;
		v->no_cacheable = 0;
		v->not_found = 0;
		
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"fstorage: mod %d %% %d=%d", n, mi->divisor, res);
		
	} else {
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
			"fstorage: mod variable %d not found", data);
		
		v->not_found = 1;
	}

	return NGX_OK;
}

/* Create local configuration */
static void* ngx_http_fstorage_create_loc_conf(ngx_conf_t *cf)
{
	ngx_http_fstorage_loc_conf_t  *conf;

	conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_fstorage_loc_conf_t));
	if (!conf)
		return NULL;

	ngx_array_init(&conf->md5_slices, cf->pool, 1, sizeof(md5_slice));
	
	ngx_array_init(&conf->modinfos, cf->pool, 1, sizeof(modinfo));

	return conf;
}

/* Merge local configuration */
static char* ngx_http_fstorage_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
	ngx_http_fstorage_loc_conf_t *prev = parent;
	ngx_http_fstorage_loc_conf_t *conf = child;
	
	if (prev->md5_slices.nelts) {
		
		// merge arrays
		memcpy(ngx_array_push_n(&conf->md5_slices, prev->md5_slices.nelts),
				prev->md5_slices.elts, sizeof(md5_slice) * prev->md5_slices.nelts);
	}

	return NGX_CONF_OK;
}

/* Command handler */
static char* ngx_http_fstorage_md5(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_fstorage_loc_conf_t *mcf = conf;
	ngx_str_t *value;
	ngx_http_variable_t *v;
	md5_slice* slice;

	// add new slice
	slice = (md5_slice*)ngx_array_push(&mcf->md5_slices);

	value = cf->args->elts;
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, 
			"fstorage: md5 command handler; src='%*s' start='%*s' num='%*s' dst='%*s'",
			value[1].len, value[1].data,
			value[2].len, value[2].data,
			value[3].len, value[3].data,
			value[4].len, value[4].data
		);

	slice->start = atoi((char*)value[2].data);
	slice->num = atoi((char*)value[3].data);
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, 
			"fstorage: md5 data start=%d; num=%d", slice->start, slice->num);

	if (slice->start > MAX_HASH_SIZE 
			|| slice->num > MAX_HASH_SIZE)
		return NGX_CONF_ERROR;
	
	if (value[1].data[0] != '$')
		return NGX_CONF_ERROR;

	value[1].data++;
	value[1].len--;

	slice->index_src = ngx_http_get_variable_index(cf, &value[1]);
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, 
		"fstorage: md5 source var index: %d", slice->index_src);
	
	if (slice->index_src == NGX_ERROR) {
		return NGX_CONF_ERROR;
	}

	if (value[4].data[0] != '$')
		return NGX_CONF_ERROR;

	value[4].data++;
	value[4].len--;

	v = ngx_http_add_variable(cf, &value[4], NGX_HTTP_VAR_CHANGEABLE);

	v->get_handler = ngx_http_fstorage_md5_variable;
	v->data = mcf->md5_slices.nelts - 1;
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, 
		"fstorage: md5 dst variable: %d", v->data);

	return NGX_CONF_OK;
}

static char* ngx_http_fstorage_mod(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_fstorage_loc_conf_t *mcf = conf;
	ngx_str_t *value;
	ngx_http_variable_t *v;
	modinfo* minfo;

	// add new modinfo
	minfo = (modinfo*)ngx_array_push(&mcf->modinfos);

	value = cf->args->elts;
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, 
			"fstorage: mod command handler; src='%*s' divisor='%*s' dst='%*s'",
			value[1].len, value[1].data,
			value[2].len, value[2].data,
			value[3].len, value[3].data
		);

	minfo->divisor = atoi((char*)value[2].data);
	
	if (!minfo->divisor) {
		ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, "fstorage: divisor is zero!");
		return NGX_CONF_ERROR;
	}
	
	if (value[1].data[0] != '$')
		return NGX_CONF_ERROR;

	value[1].data++;
	value[1].len--;

	minfo->index_src = ngx_http_get_variable_index(cf, &value[1]);
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, 
		"md5: source var index: %d", minfo->index_src);
	
	if (minfo->index_src == NGX_ERROR) {
		return NGX_CONF_ERROR;
	}

	if (value[3].data[0] != '$')
		return NGX_CONF_ERROR;

	value[3].data++;
	value[3].len--;

	v = ngx_http_add_variable(cf, &value[3], NGX_HTTP_VAR_CHANGEABLE);

	v->get_handler = ngx_http_fstorage_mod_variable;
	v->data = mcf->modinfos.nelts - 1;
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, "fstorage: mod dst variable: %d", v->data);

	return NGX_CONF_OK;
}

static ngx_int_t ngx_let_toi(ngx_str_t* s) 
{
	return (s->len > 2 && s->data[0] == '0' && s->data[1] == 'x')

		? ngx_hextoi(s->data + 2, s->len - 2)

		: ngx_atoi(s->data, s->len);
}

/* processes positive numbers only */
static ngx_int_t ngx_let_apply_binary_integer_op(ngx_http_request_t *r, int op, 
		ngx_array_t* args, ngx_str_t* value)
{
	ngx_str_t* str;
	int orig_left, left, right;
	unsigned sz;

	if (args->nelts != 2) {
		ngx_log_debug0(NGX_LOG_INFO, r->connection->log, 0, 
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
		ngx_log_debug0(NGX_LOG_INFO, r->connection->log, 0, 
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
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
					"let unexpected operation '%c'", op);
			return NGX_ERROR;
	}
	
	value->len = 64; /*TODO: better size? */
	value->data = ngx_palloc(r->pool, value->len);
	
	sz = snprintf((char*)value->data, value->len, "%d", left);

	if (sz < value->len)
		value->len = sz;
	
	ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
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
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
				"let NULL node");
		return NGX_ERROR;
	}

	switch(node->type) {
		
		case NGX_LTYPE_VARIABLE:
			
			vv = ngx_http_get_indexed_variable(r, node->index);
				
			if (vv == NULL || vv->not_found) {
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let variable %d not found", node->index);
					
				return NGX_ERROR;
			}

			value->data = vv->data;
			value->len = vv->len;
			
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let getting variable %d: '%*s'", node->index, value->len, value->data);

			break;
			
		case NGX_LTYPE_LITERAL:
			
			*value = node->name;
			
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let getting literal: '%*s'", value->len, value->data);
			
			break;
			
		case NGX_LTYPE_OPERATION:
			
			ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
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
				
				ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, 
						"let %d strings concatenated '%*s'", args.nelts, value->len, value->data);
			}

			break;
	}

	return NGX_OK;
}

static ngx_int_t ngx_http_fstorage_let_variable(ngx_http_request_t *r,
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
			
		ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "let variable accessed");
	}

	return ret;
}

static char* ngx_http_fstorage_let(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_str_t *value;
	ngx_http_variable_t *v;
	
	value = cf->args->elts;
	
	ngx_log_debug0(NGX_LOG_INFO, cf->log, 0, "let command handler");

	if (value[1].data[0] != '$')
		return NGX_CONF_ERROR;

	value[1].data++;
	value[1].len--;

	v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);

	v->get_handler = ngx_http_fstorage_let_variable;
	v->data = (uintptr_t)ngx_parse_let_expr(cf);
	
	return NGX_CONF_OK;
}

