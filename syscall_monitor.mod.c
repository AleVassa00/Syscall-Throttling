#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x092a35a2, "_copy_from_user" },
	{ 0xc87f4bab, "finish_wait" },
	{ 0xa1dacb42, "class_destroy" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0x0db8d68d, "prepare_to_wait_event" },
	{ 0x16ab4215, "__wake_up" },
	{ 0xd272d446, "__fentry__" },
	{ 0x5a844b26, "__x86_indirect_thunk_rax" },
	{ 0xe8213e80, "_printk" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xd272d446, "schedule" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0x7a5ffe84, "init_wait_entry" },
	{ 0xe486c4b7, "device_create" },
	{ 0x653aa194, "class_create" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0xc609ff70, "strncpy" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0x5403c125, "__init_waitqueue_head" },
	{ 0x888b8f57, "strcmp" },
	{ 0x6f8082dd, "pv_ops" },
	{ 0x37031a65, "__register_chrdev" },
	{ 0x1595e410, "device_destroy" },
	{ 0xc064623f, "__kmalloc_cache_noprof" },
	{ 0x97acb853, "ktime_get" },
	{ 0x1c489eb6, "register_kprobe" },
	{ 0x7a8e92c6, "unregister_kprobe" },
	{ 0xd272d446, "BUG_func" },
	{ 0x7851be11, "__SCT__might_resched" },
	{ 0xfaabfe5e, "kmalloc_caches" },
	{ 0x52b15b3b, "__unregister_chrdev" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x092a35a2,
	0xc87f4bab,
	0xa1dacb42,
	0xcb8b6ec6,
	0x0db8d68d,
	0x16ab4215,
	0xd272d446,
	0x5a844b26,
	0xe8213e80,
	0xbd03ed67,
	0xd272d446,
	0xd272d446,
	0x90a48d82,
	0x7a5ffe84,
	0xe486c4b7,
	0x653aa194,
	0xbd03ed67,
	0xc609ff70,
	0xd272d446,
	0x5403c125,
	0x888b8f57,
	0x6f8082dd,
	0x37031a65,
	0x1595e410,
	0xc064623f,
	0x97acb853,
	0x1c489eb6,
	0x7a8e92c6,
	0xd272d446,
	0x7851be11,
	0xfaabfe5e,
	0x52b15b3b,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"_copy_from_user\0"
	"finish_wait\0"
	"class_destroy\0"
	"kfree\0"
	"prepare_to_wait_event\0"
	"__wake_up\0"
	"__fentry__\0"
	"__x86_indirect_thunk_rax\0"
	"_printk\0"
	"__ref_stack_chk_guard\0"
	"schedule\0"
	"__stack_chk_fail\0"
	"__ubsan_handle_out_of_bounds\0"
	"init_wait_entry\0"
	"device_create\0"
	"class_create\0"
	"random_kmalloc_seed\0"
	"strncpy\0"
	"__x86_return_thunk\0"
	"__init_waitqueue_head\0"
	"strcmp\0"
	"pv_ops\0"
	"__register_chrdev\0"
	"device_destroy\0"
	"__kmalloc_cache_noprof\0"
	"ktime_get\0"
	"register_kprobe\0"
	"unregister_kprobe\0"
	"BUG_func\0"
	"__SCT__might_resched\0"
	"kmalloc_caches\0"
	"__unregister_chrdev\0"
	"module_layout\0"
;

MODULE_INFO(depends, "");


MODULE_INFO(srcversion, "C431940E483AEF8F03281E8");
