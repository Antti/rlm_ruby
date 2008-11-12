/*
 * rlm_ruby.c
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 *  Copyright 2008 Andriy Dmytrenko aka Antti, BuzhNET
 */

#include <freeradius-devel/ident.h>
RCSID("$Id: rlm_ruby.c,v 0.1 2008/10/22 12:54:00 antti Exp $")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>

#include <ruby.h>

/*
 *	Define a structure for our module configuration.
 *
 *	These variables do not need to be in a structure, but it's
 *	a lot cleaner to do so, and a pointer to the structure can
 *	be used as the instance handle.
 */
typedef struct rlm_ruby_t {

#define RLM_RUBY_STRUCT(foo) int func_##foo

	RLM_RUBY_STRUCT(instantiate);
	RLM_RUBY_STRUCT(authorize);
	RLM_RUBY_STRUCT(authenticate);
	RLM_RUBY_STRUCT(preacct);
	RLM_RUBY_STRUCT(accounting);
	RLM_RUBY_STRUCT(checksimul);
	RLM_RUBY_STRUCT(preproxy);
	RLM_RUBY_STRUCT(postproxy);
	RLM_RUBY_STRUCT(postauth);
	RLM_RUBY_STRUCT(detach);
	
	char *scriptFile;
	char *moduleName;
	VALUE pModule_builtin;

} rlm_ruby_t;


/*
 *	A mapping of configuration file names to internal variables.
 *
 *	Note that the string is dynamically allocated, so it MUST
 *	be freed.  When the configuration file parse re-reads the string,
 *	it free's the old one, and strdup's the new one, placing the pointer
 *	to the strdup'd string into 'config.string'.  This gets around
 *	buffer over-flows.
 */


static const CONF_PARSER module_config[] = {
    { "scriptfile",   PW_TYPE_FILENAME,
    offsetof(struct rlm_ruby_t, scriptFile), NULL,  NULL },
    { "modulename",   PW_TYPE_STRING_PTR,
     offsetof(struct rlm_ruby_t, moduleName), NULL,  "Radiusd" },
    { NULL, -1, 0, NULL, NULL } /* end of module_config */
};
																	     

/*
 * radiusd Ruby functions
 */

/* radlog wrapper */

static VALUE radlog_rb(VALUE self,VALUE msg_type, VALUE rb_msg) {
    int status;
    char *msg;
    status = FIX2INT(msg_type);
    msg = STR2CSTR(rb_msg);
    radlog(status, msg);
    return Qnil;
}


/* Tuple to value pair conversion */

static void add_vp_tuple(VALUE_PAIR **vpp, VALUE rb_value,
			 const char *function_name) {
    int i, outertuplesize;
    VALUE_PAIR	*vp;

    /* If the Ruby function gave us nil for the tuple, then just return. */
    if (NIL_P(rb_value)) {
	return;
    }

    if (TYPE(rb_value)!=T_ARRAY) {
	radlog(L_ERR, "add_vp_tuple, %s: non-array passed", function_name);
	return;
    }

    /* Get the array size. */
    outertuplesize = RARRAY(rb_value)->len;

    for (i = 0; i < outertuplesize; i++) {
	VALUE pTupleElement = rb_ary_entry(rb_value, i);

	if ((pTupleElement !=0) &&
	    (TYPE(pTupleElement)==T_ARRAY)) {

	    /* Check if it's a pair */
	    int tuplesize;

	    if ((tuplesize = RARRAY(pTupleElement)->len) != 2) {
		radlog(L_ERR, "%s: tuple element %d is a tuple "
		       " of size %d. must be 2\n", function_name,
		       i, tuplesize);
	    }
	    else {
		VALUE pString1, pString2;

		pString1 = rb_ary_entry(pTupleElement, 0);
		pString2 = rb_ary_entry(pTupleElement, 1);

		if  ((TYPE(pString1)==T_STRING) &&
		     (TYPE(pString2)==T_STRING)) {


		    const char *s1, *s2;

		    /* pairmake() will convert and find any
		     * errors in the pair.
		     */

		    s1 = STR2CSTR(pString1);
		    s2 = STR2CSTR(pString2);

		    if ((s1 != NULL) && (s2 != NULL)) {
			radlog(L_DBG, "%s: %s = %s ",
			       function_name, s1, s2);

			/* xxx Might need to support other T_OP */
			vp = pairmake(s1, s2, T_OP_EQ);
			if (vp != NULL) {
			    pairadd(vpp, vp);
			    radlog(L_DBG, "%s: s1, s2 OK\n",
				   function_name);
			}
			else {
			    radlog(L_DBG, "%s: s1, s2 FAILED\n",
				   function_name);
			}
		    }
		    else {
			radlog(L_ERR, "%s: string conv failed\n",
			       function_name);
		    }

		}
		else {
		    radlog(L_ERR, "%s: tuple element %d must be "
			   "(string, string)", function_name, i);
		}
	    }
	}
	else {
	    radlog(L_ERR, "%s: tuple element %d is not a tuple\n",
		   function_name, i);
	}
    }

}

/* This is the core Ruby function that the others wrap around.
 * Pass the value-pair print strings in a tuple.
 * xxx We're not checking the errors. If we have errors, what do we do?
 */

static int ruby_function(REQUEST *request, int func, VALUE module, const char *function_name)
{
#define BUF_SIZE 1024

    char buf[BUF_SIZE];		/* same size as vp_print buffer */

    VALUE_PAIR	*vp;
    VALUE rb_request, rb_result,rb_reply_items,rb_config_items;

    int n_tuple, return_value;
    radlog(L_ERR, "Calling ruby function %s which has id: %d\n",function_name,func);

    /* Return with "OK, continue" if the function is not defined. 
     * TODO: Should check with rb_respond_to each time, just because ruby can define function dynamicly?
     */
    if (func == 0) {
	return RLM_MODULE_OK;
    }

    /* Default return value is "OK, continue" */
    return_value = RLM_MODULE_OK;

    n_tuple = 0;

    if (request != NULL) {
	for (vp = request->packet->vps; vp; vp = vp->next) {
	    n_tuple++;
	}
    }
    /*
	Creating ruby array, that contains arrays of [name,value]
	Maybe we should use hash instead? Can this names repeat?
    */
    rb_request=rb_ary_new2(n_tuple);
    if (request != NULL) {
    for (vp = request->packet->vps; vp; vp = vp->next) {	
	VALUE tmp = rb_ary_new2(2);
	
	/* The name. logic from vp_prints, lib/print.c */
        if (vp->flags.has_tag) {
            snprintf(buf, BUF_SIZE, "%s:%d", vp->name, vp->flags.tag);
        }
        else {
            strcpy(buf, vp->name);
        }
        VALUE rbString1 = rb_str_new2(buf);
        /* The value. Use delimiter - don't know what that means */
        vp_prints_value(buf, sizeof(buf), vp, 1);
        VALUE rbString2 = rb_str_new2(buf);
	
	rb_ary_push(tmp,rbString1);
	rb_ary_push(tmp,rbString2);
	rb_ary_push(rb_request,tmp);
    }
    }

    /* Calling corresponding ruby function, passing request and catching result */
    rb_result = rb_funcall(module,func,1, rb_request);
    
    /* Checking result, it can be array of type [result, [array of reply pairs],[array of config pairs]],
     * It can also be just a fixnum, which is a result itself.
     */
    if (TYPE(rb_result)==T_ARRAY)
    {
	if (!FIXNUM_P(rb_ary_entry(rb_result, 0)))
	{
	    radlog(L_ERR,"First element of an array was not a FIXNUM(Which has to be a return_value)");
	}
	else
	    return_value = FIX2INT(rb_ary_entry(rb_result, 0));
	rb_reply_items = rb_ary_entry(rb_result, 1);
	rb_config_items = rb_ary_entry(rb_result, 2);
	add_vp_tuple(&request->reply->vps,rb_reply_items, function_name);
	add_vp_tuple(&request->config_items,rb_config_items, function_name);
    }
    else if (FIXNUM_P(rb_result))
    {
	return_value = FIX2INT(rb_result);	
    }
    return return_value;
}


static struct varlookup {
	const char*	name;
	int		value;
} constants[] = {
	{ "L_DBG",		L_DBG			},
	{ "L_AUTH",		L_AUTH			},
	{ "L_INFO",		L_INFO			},
	{ "L_ERR",		L_ERR			},
	{ "L_PROXY",		L_PROXY			},
	{ "L_CONS",		L_CONS			},
	{ "RLM_MODULE_REJECT",	RLM_MODULE_REJECT	},
	{ "RLM_MODULE_FAIL",	RLM_MODULE_FAIL		},
	{ "RLM_MODULE_OK",	RLM_MODULE_OK		},
	{ "RLM_MODULE_HANDLED",	RLM_MODULE_HANDLED	},
	{ "RLM_MODULE_INVALID",	RLM_MODULE_INVALID	},
	{ "RLM_MODULE_USERLOCK",RLM_MODULE_USERLOCK	},
	{ "RLM_MODULE_NOTFOUND",RLM_MODULE_NOTFOUND	},
	{ "RLM_MODULE_NOOP",	RLM_MODULE_NOOP		},
	{ "RLM_MODULE_UPDATED",	RLM_MODULE_UPDATED	},
	{ "RLM_MODULE_NUMCODES",RLM_MODULE_NUMCODES	},
	{ NULL, 0 },
};

/*
 * Import a user module and load a function from it
 */
static int load_ruby_function(const char *f_name, int *func, VALUE module) 
{
    if (f_name==NULL)  {
	*func=0;
    } else {
	*func=rb_intern(f_name);
	/* rb_intern returns a symbol of a function, not a function itself
	    it can be aplied to any recipient,
	    so we should check it for our module recipient
	*/
	if (!rb_respond_to(module,*func))
	    *func=0;
    }
    radlog(L_DBG,"load_ruby_function %s, result: %d",f_name, *func);
    return 0;
}


/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 *
 */
static int ruby_instantiate(CONF_SECTION *conf, void **instance)
{
    rlm_ruby_t *data;
    VALUE module;
    int idx;

    /*
     * Initialize Ruby interpreter. Fatal error if this fails.
     */
     
    radlog(L_DBG,"[rlm_ruby]: ruby_instantiate");
    ruby_init();
    ruby_init_loadpath();
    ruby_script("radiusd");
    int status;
    /*
     *	Set up a storage area for instance data
     */
    data = rad_malloc(sizeof(*data));
    if (!data) {
      return -1;
    }
    memset(data, 0, sizeof(*data));
    
    /*
     *	If the configuration parameters can't be parsed, then
     *	fail.
    */
    if (cf_section_parse(conf, data, module_config) < 0) {
	free(data);
	return -1;
    }

    /*
     * Setup our 'radiusd' module.
     */
    
    if ((module = data->pModule_builtin = rb_define_module(data->moduleName)) == 0) {
	radlog(L_ERR, "Ruby rb_define_module failed");
	free(data);
	return -1;
    }
    /*
     * Load constants into module
     */
    for (idx=0; constants[idx].name; idx++)
	rb_define_const(module, constants[idx].name, INT2NUM(constants[idx].value));

    /* Add functions into module */
    rb_define_module_function(module, "radlog", radlog_rb, 2);
    
    if (data->scriptFile==NULL)
    {
	/* TODO: What actualy should we do? Exit with module fail? Or continue... but what the point then? */
	radlog(L_ERR,"Script File was not set");
    }
    else
    {
    radlog(L_DBG,"Loading file %s...",data->scriptFile);
    rb_load_protect(rb_str_new2(data->scriptFile), 0, &status);
    if (!status)
	radlog(L_DBG,"Loaded file %s",data->scriptFile);
    else
	radlog(L_ERR,"Error loading file %s status: %d",data->scriptFile ,status);
    }
    /*
     * Import user modules.
     */
#define RLM_RUBY_LOAD(foo) if (load_ruby_function(#foo, &data->func_##foo, data->pModule_builtin)==-1) { \
	/* TODO: check if we need to cleanup data */ \
	return -1; \
    }

    RLM_RUBY_LOAD(instantiate);
    RLM_RUBY_LOAD(authenticate);
    RLM_RUBY_LOAD(authorize);
    RLM_RUBY_LOAD(preacct);
    RLM_RUBY_LOAD(accounting);
    RLM_RUBY_LOAD(checksimul);
    RLM_RUBY_LOAD(preproxy);
    RLM_RUBY_LOAD(postproxy);
    RLM_RUBY_LOAD(postauth);
    RLM_RUBY_LOAD(detach);

    *instance=data;

    /* Call the instantiate function.  No request.  Use the return value. */
    return ruby_function(NULL, data->func_instantiate,data->pModule_builtin, "instantiate");
}

#define RLM_RUBY_FUNC(foo) static int ruby_##foo(void *instance, REQUEST *request) \
{ \
    return ruby_function(request, \
			   ((struct rlm_ruby_t *)instance)->func_##foo,((struct rlm_ruby_t *)instance)->pModule_builtin, \
			   #foo); \
}


RLM_RUBY_FUNC(authorize)
RLM_RUBY_FUNC(authenticate)
RLM_RUBY_FUNC(preacct)
RLM_RUBY_FUNC(accounting)
RLM_RUBY_FUNC(checksimul)
RLM_RUBY_FUNC(preproxy)
RLM_RUBY_FUNC(postproxy)
RLM_RUBY_FUNC(postauth)

static int ruby_detach(void *instance)
{
    int return_value;

    /* Default return value is failure */
    return_value = -1;
    free(instance);
    ruby_finalize();
    ruby_cleanup(0);
    radlog(L_DBG, "ruby_detach done");
    
    //Ok, we cheat, and returon ok value for now :)
    return_value = RLM_MODULE_OK;

    /* Return the specified by the Ruby module */
    return return_value;
}


/*
 *	The module name should be the only globally exported symbol.
 *	That is, everything else should be 'static'.
 *
 *	If the module needs to temporarily modify it's instantiation
 *	data, the type should be changed to RLM_TYPE_THREAD_UNSAFE.
 *	The server will then take care of ensuring that the module
 *	is single-threaded.
 */
module_t rlm_ruby = {
	RLM_MODULE_INIT,
	"ruby",
//	RLM_TYPE_THREAD_SAFE,		/* type */
	RLM_TYPE_THREAD_UNSAFE,		/* type, ok, let's be honest, MRI is not yet treadsafe */
	ruby_instantiate,		/* instantiation */
	ruby_detach,			/* detach */
	{
		ruby_authenticate,	/* authentication */
		ruby_authorize,	/* authorization */
		ruby_preacct,		/* preaccounting */
		ruby_accounting,	/* accounting */
		ruby_checksimul,	/* checksimul */
		ruby_preproxy,	/* pre-proxy */
		ruby_postproxy,	/* post-proxy */
		ruby_postauth		/* post-auth */
	},
};
