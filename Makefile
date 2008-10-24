
#TODO: We should get those ldflags and cflags somewhere

TARGET     = rlm_ruby
SRCS       = rlm_ruby.c
HEADERS    = @ruby_include_dir@
RLM_LIBS   = @ruby_ldflags@
RLM_CFLAGS = @ruby_cflags@

include ../rules.mak

$(LT_OBJS): $(HEADERS)
