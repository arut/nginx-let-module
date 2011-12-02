#ifndef __NGINX_HTTP_LET_H__
#define __NGINX_HTTP_LET_H__

#include <ngx_core.h>

/* node types */
#define NGX_LTYPE_VARIABLE  1
#define NGX_LTYPE_LITERAL   2
#define NGX_LTYPE_OPERATION 3

struct ngx_let_node_s {
	
	ngx_int_t type;
	
	union {

		ngx_str_t name;   /* literal value / operation name */

		ngx_int_t index;  /* variable index */
	};

	ngx_array_t args; /* argument node pointers */
};

typedef struct ngx_let_node_s ngx_let_node_t;

/* parses let expression & returns to node pointer */
ngx_let_node_t* ngx_parse_let_expr(ngx_conf_t* cf);

#endif /* __NGINX_HTTP_LET_H__ */
