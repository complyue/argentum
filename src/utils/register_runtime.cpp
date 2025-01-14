#include "utils/register_runtime.h"

#include <vector>
#include <cstring>

#include "compiler/ast.h"
#include "runtime/runtime.h"

void register_runtime_content(struct ast::Ast& ast) {
	if (ast.object)
		return;
#ifdef AG_STANDALONE_COMPILER_MODE
#define FN(A) (void(*)())(nullptr)
#else
	using FN = void(*)();
#endif
	using mut = ast::Mut;
	ast.object = ast.mk_class("Object");
	auto container = ast.mk_class("Container", {
		ast.mk_field("_size", new ast::ConstInt64),
		ast.mk_field("_data", new ast::ConstInt64) });
	ast.mk_method(mut::ANY, container, "capacity", FN(ag_m_sys_Container_capacity), new ast::ConstInt64, {});
	ast.mk_method(mut::MUTATING, container, "insertItems", FN(&ag_m_sys_Container_insertItems), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, container, "moveItems", FN(&ag_m_sys_Container_moveItems), new ast::ConstBool, { ast.tp_int64(), ast.tp_int64(), ast.tp_int64() });

	ast.blob = ast.mk_class("Blob");
	ast.blob->overloads[container];
	ast.mk_method(mut::ANY, ast.blob, "get8At", FN(ag_m_sys_Blob_get8At), new ast::ConstInt64, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set8At", FN(ag_m_sys_Blob_set8At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::ANY, ast.blob, "get16At", FN(ag_m_sys_Blob_get16At), new ast::ConstInt64, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set16At", FN(ag_m_sys_Blob_set16At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::ANY, ast.blob, "get32At", FN(ag_m_sys_Blob_get32At), new ast::ConstInt64, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set32At", FN(ag_m_sys_Blob_set32At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::ANY, ast.blob, "get64At", FN(ag_m_sys_Blob_get64At), new ast::ConstInt64, { ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "set64At", FN(ag_m_sys_Blob_set64At), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "deleteBytes", FN(ag_m_sys_Blob_deleteBytes), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "copyBytesTo", FN(ag_m_sys_Blob_copyBytesTo), new ast::ConstBool, { ast.tp_int64(), ast.get_conform_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.blob, "putChAt", FN(ag_m_sys_Blob_putChAt), new ast::ConstInt64, { ast.tp_int64(), ast.tp_int64() });

	ast.str_builder = ast.mk_class("StrBuilder");
	ast.str_builder->overloads[ast.blob];

	auto inst = new ast::MkInstance;
	inst->cls = ast.object.pinned();
	auto ref_to_object = new ast::RefOp;
	ref_to_object->p = inst;
	auto opt_ref_to_object = new ast::If;
	opt_ref_to_object->p[0] = new ast::ConstBool;
	opt_ref_to_object->p[1] = ref_to_object;
	auto weak_to_object = new ast::MkWeakOp;
	weak_to_object->p = inst;
	auto add_class_param = [&](ltm::pin<ast::Class> cls) {
		auto param = ltm::pin<ast::ClassParam>::make();
		cls->params.push_back(param);
		param->base = ast.object;
		param->name = "T";
		return param;
	};
	auto make_ptr_result = [&](ltm::pin<ast::UnaryOp> typer, ltm::pin<ast::AbstractClass> cls) {
		auto inst_t = new ast::MkInstance;
		inst_t->cls = cls;
		typer->p = inst_t;
		return typer;
	};
	auto make_opt_result = [&](ltm::pin<ast::Action> ref) {
		auto opt_ref_to_t = new ast::If;
		opt_ref_to_t->p[0] = new ast::ConstBool;
		opt_ref_to_t->p[1] = ref;
		return opt_ref_to_t;
	};
	ast.own_array = ast.mk_class("Array");
	ast.own_array->overloads[container];
	{
		auto t_cls = add_class_param(ast.own_array);
		auto ref_to_t_res = make_ptr_result(new ast::RefOp, t_cls);
		auto opt_ref_to_t_res = make_opt_result(ref_to_t_res);
		auto own_to_t = ast.get_own(t_cls);
		auto opt_own_to_t = ast.tp_optional(own_to_t);
		ast.mk_method(mut::ANY, ast.own_array, "getAt", FN(ag_m_sys_Array_getAt), opt_ref_to_t_res, { ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.own_array, "setAt", FN(ag_m_sys_Array_setAt), ref_to_t_res, { ast.tp_int64(), own_to_t });
		ast.mk_method(mut::MUTATING, ast.own_array, "setOptAt", FN(ag_m_sys_Array_setOptAt), new ast::ConstVoid, { ast.tp_int64(), opt_own_to_t });
		ast.mk_method(mut::MUTATING, ast.own_array, "delete", FN(ag_m_sys_Array_delete), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.own_array, "spliceAt", FN(ag_m_sys_Array_spliceAt), new ast::ConstBool, { ast.tp_int64(), ast.tp_optional(ast.get_ref(t_cls)) });
	}
	ast.weak_array = ast.mk_class("WeakArray");
	ast.weak_array->overloads[container];
	{
		auto t_cls = add_class_param(ast.weak_array);
		ast.mk_method(mut::ANY, ast.weak_array, "getAt", FN(ag_m_sys_WeakArray_getAt), make_ptr_result(new ast::MkWeakOp, t_cls), { ast.tp_int64() });
		ast.mk_method(mut::MUTATING, ast.weak_array, "setAt", FN(ag_m_sys_WeakArray_setAt), new ast::ConstVoid, { ast.tp_int64(), ast.get_weak(t_cls) });
		ast.mk_method(mut::MUTATING, ast.weak_array, "delete", FN(ag_m_sys_WeakArray_delete), new ast::ConstVoid, { ast.tp_int64(), ast.tp_int64() });
	}
	ast.string_cls = ast.mk_class("String", {
		ast.mk_field("_cursor", new ast::ConstInt64),
		ast.mk_field("_buffer", new ast::ConstInt64) });
	ast.mk_method(mut::MUTATING, ast.string_cls, "fromBlob", FN(ag_m_sys_String_fromBlob), new ast::ConstBool, { ast.get_conform_ref(ast.blob), ast.tp_int64(), ast.tp_int64() });
	ast.mk_method(mut::MUTATING, ast.string_cls, "getCh", FN(ag_m_sys_String_getCh), new ast::ConstInt64, {});
	
	ast.mk_fn("getParent", FN(ag_fn_sys_getParent), opt_ref_to_object, { ast.get_conform_ref(ast.object) });
	ast.mk_fn("log", FN(ag_fn_sys_log), new ast::ConstVoid, { ast.get_conform_ref(ast.string_cls) });
	ast.mk_fn("terminate", FN(ag_fn_sys_terminate), new ast::ConstVoid, { ast.tp_int64() });
	ast.mk_fn("setMainObject", FN(ag_fn_sys_setMainObject), new ast::ConstVoid, { ast.tp_optional(ast.get_ref(ast.object))});
	ast.mk_fn("postTimer", FN(ag_fn_sys_postTimer), new ast::ConstVoid, {
		ast.tp_int64(),
		ast.tp_delegate({ ast.tp_void() })
	});
	auto thread = ast.mk_class("Thread", {
		ast.mk_field("_internal", new ast::ConstInt64) });
	{
		auto start = ast.mk_method(mut::MUTATING, thread, "start", FN(ag_m_sys_Thread_start), nullptr, { ast.get_ref(ast.object) });
		start->is_factory = true;
		auto get_this = new ast::Get;
		start->type_expression = get_this;
		get_this->var = start->names[0];
	}
	ast.mk_method(mut::MUTATING, thread, "root", FN(ag_m_sys_Thread_root), make_ptr_result(new ast::MkWeakOp, ast.object), {});

	ast.platform_exports.insert({
		{ "ag_init", FN(ag_init) },
		{ "ag_copy", FN(ag_copy) },
		{ "ag_copy_object_field", FN(ag_copy_object_field) },
		{ "ag_copy_weak_field", FN(ag_copy_weak_field) },
		{ "ag_allocate_obj", FN(ag_allocate_obj) },
		{ "ag_mk_weak", FN(ag_mk_weak) },
		{ "ag_deref_weak", FN(ag_deref_weak) },
		{ "ag_reg_copy_fixer", FN(ag_reg_copy_fixer) },
		{ "ag_retain_own", FN(ag_retain_own) },
		{ "ag_retain_shared", FN(ag_retain_shared) },
		{ "ag_retain_weak", FN(ag_retain_weak) },
		{ "ag_release_own", FN(ag_release_own) },
		{ "ag_release_shared", FN(ag_release_shared) },
		{ "ag_release_pin", FN(ag_release_pin) },
		{ "ag_release_weak", FN(ag_release_weak) },
		{ "ag_dispose_obj", FN(ag_dispose_obj) },
		{ "ag_set_parent", FN(ag_set_parent) },
		{ "ag_splice", FN(ag_splice) },
		{ "ag_freeze", FN(ag_freeze) },
		{ "ag_unlock_thread_queue", FN(ag_unlock_thread_queue) }, // used in trampoline
		{ "ag_get_thread_param", FN(ag_get_thread_param) }, // used in trampoline
		{ "ag_prepare_post_message", FN(ag_prepare_post_message) }, // used in post~message
		{ "ag_put_thread_param", FN(ag_put_thread_param) }, // used in post~message
		{ "ag_put_thread_param_weak_ptr", FN(ag_put_thread_param_weak_ptr) }, // used in post~message
		{ "ag_put_thread_param_own_ptr", FN(ag_put_thread_param_own_ptr) }, // used in post~message
		{ "ag_finalize_post_message", FN(ag_finalize_post_message) }, // used in post~message
		{ "ag_handle_main_thread", FN(ag_handle_main_thread) },

		{ "ag_copy_sys_Container", FN(ag_copy_sys_Container) },
		{ "ag_dtor_sys_Container", FN(ag_dtor_sys_Container) },
		{ "ag_visit_sys_Container", FN(ag_visit_sys_Container) },
		{ "ag_copy_sys_Blob", FN(ag_copy_sys_Blob) },
		{ "ag_dtor_sys_Blob", FN(ag_dtor_sys_Blob) },
		{ "ag_visit_sys_Blob", FN(ag_visit_sys_Blob) },
		{ "ag_copy_sys_Array", FN(ag_copy_sys_Array) },
		{ "ag_dtor_sys_Array",FN(ag_dtor_sys_Array) },
		{ "ag_visit_sys_Array",FN(ag_visit_sys_Array) },
		{ "ag_copy_sys_WeakArray", FN(ag_copy_sys_WeakArray) },
		{ "ag_dtor_sys_WeakArray", FN(ag_dtor_sys_WeakArray) },
		{ "ag_visit_sys_WeakArray", FN(ag_visit_sys_WeakArray) },
		{ "ag_copy_sys_String", FN(ag_copy_sys_String) },
		{ "ag_dtor_sys_String", FN(ag_dtor_sys_String) },
		{ "ag_visit_sys_String", FN(ag_visit_sys_String) },
		{ "ag_copy_sys_Thread", FN(ag_copy_sys_Thread) },
		{ "ag_dtor_sys_Thread", FN(ag_dtor_sys_Thread) },
		{ "ag_visit_sys_Thread", FN(ag_visit_sys_Thread) } });
}
