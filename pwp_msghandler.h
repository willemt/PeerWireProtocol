
void* pwp_msghandler_new(void *pc);
void pwp_msghandler_release(void *pc);
void pwp_msghandler_dispatch_from_buffer(void *mh, const unsigned char* buf, unsigned int len);

