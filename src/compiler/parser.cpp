#include "compiler/parser.h"

#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <algorithm>
#include <variant>
#include <optional>
#include <cfenv>
#include <cmath>
#include "utils/utf8.h"

namespace {

using std::string;
using std::unordered_map;
using std::unordered_set;
using std::vector;
using std::variant;
using std::optional;
using std::nullopt;
using std::get_if; 
using std::pow;
using ltm::weak;
using ltm::own;
using ltm::pin;
using dom::Name;
using ast::Ast;
using ast::Node;
using ast::Action;
using ast::make_at_location;
using module_text_provider_t = const std::function<string (string name)>&;

template<typename FN>
struct Guard{
	FN fn;
	Guard(FN fn) : fn(std::move(fn)) {}
	~Guard() { fn(); }
};
template<typename FN>
Guard<FN> mk_guard(FN fn) { return Guard<FN>(std::move(fn)); }

struct Parser {
	pin<dom::Dom> dom;
	pin<Ast> ast;
	string text;
	string module_name;
	pin<ast::Module> module;
	int32_t pos = 1;
	int32_t line = 1;
	const char* cur = nullptr;
	unordered_set<string>& modules_in_dep_path;
	unordered_map<string, pin<ast::ImmediateDelegate>> delegates;
	pin<ast::Class> current_class;  // To match type parameters

	Parser(pin<Ast> ast, string module_name, unordered_set<string>& modules_in_dep_path)
		: dom(ast->dom)
		, ast(ast)
		, module_name(module_name)
		, modules_in_dep_path(modules_in_dep_path)
	{}

	void parse_fn_def(pin<ast::Function> fn) {
		expect("(");
		while (!match(")")) {
			auto param = make<ast::Var>();
			fn->names.push_back(param);
			param->initializer = parse_type();
			param->name = expect_id("parameter name");
			if (match(")"))
				break;
			expect(",");
		}
		auto as_method = dom::strict_cast<ast::Method>(fn);
		if (match("this")) {
			if (as_method)
				as_method->is_factory = true;
			else
				error("only methods return this type.");
			auto get_this = make<ast::Get>();
			get_this->var = fn->names[0];
			fn->type_expression = get_this;
			expect("{");
		} else {
			if (match(";")) {
				fn->type_expression = make<ast::ConstVoid>();
				fn->is_platform = true;
				return;
			}
			if (match("{")) {
				fn->type_expression = make<ast::ConstVoid>();
			} else {
				fn->type_expression = parse_type();
				if (match(";")) {
					fn->is_platform = true;
					return;
				}
				expect("{");
			}
		}
		parse_statement_sequence(fn->body);
		if (as_method && as_method->is_factory) {
			fn->body.push_back(fn->type_expression);  // this
		}
		expect("}");
	}
	void add_this_param(ast::Function& fn, pin<ast::Class> cls) {
		auto this_param = make<ast::Var>();
		fn.names.push_back(this_param);
		this_param->name = "this";
		auto this_init = make<ast::MkInstance>();
		this_init->cls = cls;
		this_param->initializer = this_init;
	}
	pin<ast::Method> make_method(const ast::LongName& name, pin<ast::Class> cls, bool is_interface) {
		auto method = make<ast::Method>();
		method->name = name.name;
		method->base_module = name.module;
		add_this_param(*method, cls);
		parse_fn_def(method);
		if (is_interface != method->body.empty()) {
			error(is_interface ? "empty" : "not empty", " body expected");
		}
		return method;
	}
	ast::LongName expect_long_name(char* message, pin<ast::Module> def_module) {
		auto id = expect_id(message);
		if (!match("_"))
			return { id, def_module };
		if (auto it = module->direct_imports.find(id); it != module->direct_imports.end())
			return { expect_id(message), it->second };
		if (id == module->name)
			error("names of the current module should not be prefixed with a module name");
		else
			error("module ", id, " is not visible from module ", module->name);
	}
	pin<ast::Class> get_class_by_name(char* message) {
		auto [c_name, m] = expect_long_name(message, nullptr);
		if (!m) {
			if (auto it = module->aliases.find(c_name); it != module->aliases.end()) {
				if (auto as_cls = dom::strict_cast<ast::Class>(it->second))
					return as_cls;
			}
			m = module;
		}
		return m->get_class(c_name);
	}
	pin<ast::Module> parse(module_text_provider_t module_text_provider)
	{
		if (modules_in_dep_path.count(module_name) != 0) {
			string msg = "curcular dependency in modules:";
			for (auto& m : modules_in_dep_path)
				msg += m + " ";
			error(msg);
		}
		if (auto it = ast->modules.find(module_name); it != ast->modules.end())
				return it->second;
		module = new ast::Module;
		module->name = module_name;
		if (module_name != "sys")
			module->direct_imports.insert({ "sys", ast->sys });
		ast->modules.insert({ module_name, module });
		modules_in_dep_path.insert(module_name);
		text = module_text_provider(module_name);
		cur = text.c_str();
		match_ws();
		while (match("using")) {
			auto using_name = expect_id("imported module");
			auto used_module = using_name == "sys"
				? ast->sys.pinned()
				: Parser(ast, using_name, modules_in_dep_path).parse(module_text_provider);
			module->direct_imports.insert({ using_name, used_module });
			if (match("{")) {
				do {
					auto my_id = expect_id("alias name");
					auto their_id = my_id;
					if (match("=")) {
						their_id = expect_id("name in package");
					}
					if (auto it = used_module->functions.find(their_id); it != used_module->functions.end())
						module->aliases.insert({ my_id, it->second });
					else if (auto it = used_module->classes.find(their_id); it != used_module->classes.end())
						module->aliases.insert({ my_id, it->second });
					else
						error("unknown name ", their_id, " in module ", using_name);
					expect(";");
				} while (!match("}"));
			} else {
				expect(";");
			}
		}
		ast->modules_in_order.push_back(module);
		for (;;) {
			if (match("const")) {
				string id = expect_id("const name");
				expect("=");
				auto v = make<ast::Var>();
				v->initializer = parse_expression();
				v->name = id;
				v->is_const = true;
				module->constants.insert({ id, v });
				expect(";");
				continue;
			}
			bool is_test = match("test");
			bool is_interface = match("interface");
			if (is_interface || match("class")) {
				auto cls = get_class_by_name("class or interface");
				current_class = cls;
				bool is_first_time_seen = cls->line == 0;
				cls->line = line;
				cls->pos = pos;
				// TODO match attributes if existed
				cls->is_interface = is_interface;
				cls->is_test = is_test;
				if (match("(")) {
					if (!is_first_time_seen)
						error("Reopened class must reuse existing type parameters");
					do {
						auto param = make<ast::ClassParam>();
						param->name = expect_id("type parameter name");
						if (match(">"))
							param->is_out = false;
						else if (match("<"))
							param->is_in = false;
						param->base = get_class_by_name("base class for type parameter");
						cls->params.push_back(param);
					} while (match(","));
					expect(")");
				}
				expect("{");
				while (!match("}")) {
					if (match("+")) {
						auto base_class = get_class_by_name("base class or interface");
						auto& base_content = cls->overloads[base_class];
						if (match("{")) {
							if (is_interface)
								error("interface can't have overrides");
							while (!match("}"))
								base_content.push_back(make_method(expect_long_name("override method name", nullptr), cls, is_interface));
						} else {
							expect(";");
						}
					} else {
						int is_mut = match("*") ? -1 :
							match("-") ? 0 : 1;
						auto member_name = expect_id("method or field name");
						if (match("=")) {
							if (is_mut != 1)
								error("field can't have '-' or '*' markers");
							cls->fields.push_back(make<ast::Field>());
							cls->fields.back()->name = member_name;
							cls->fields.back()->initializer = parse_expression();
							expect(";");
						} else {
							cls->new_methods.push_back(make_method({ member_name, module }, cls, is_interface));
							cls->new_methods.back()->mut = is_mut;
						}
					}
				}
				current_class = nullptr;
			} else if (match("fn")) {
				auto fn = make<ast::Function>();
				fn->name = expect_id("function name");
				fn->is_test = is_test;
				auto& fn_ref = module->functions[fn->name];
				if (fn_ref)
					error("duplicated function name, ", fn->name, " see ", *fn_ref.pinned());
				fn_ref = fn;
				parse_fn_def(fn);
			} else if (is_test) {
				auto fn = make<ast::Function>();
				fn->name = expect_id("test name");
				fn->is_test = true;
				auto& fn_ref = module->tests[fn->name];
				if (fn_ref)
					error("duplicated test name, ", fn->name, " see ", *fn_ref.pinned());
				fn_ref = fn;
				parse_fn_def(fn);
			} else {
				break;
			}
		}
		module->entry_point = make<ast::Function>();
		if (*cur)
			parse_statement_sequence(module->entry_point->body);
		if (*cur)
			error("unexpected statements");
		modules_in_dep_path.erase(module_name);
		return module;
	}

	void parse_statement_sequence(vector<own<Action>>& body) {
		do {
			if (*cur == '}' || !*cur) {
				body.push_back(make<ast::ConstVoid>());
				break;
			}
			body.push_back(parse_statement());
		} while (match(";"));
	}
	pin<ast::Action> mk_get(char* kind) {
		auto n = expect_long_name(kind, nullptr);
		if (!n.module && current_class) {
			for (auto& p : current_class->params) {
				if (n.name == p->name) {
					auto inst = make<ast::MkInstance>();
					inst->cls = p;
					return inst;
				}
			}
		}
		auto get = make<ast::Get>();
		get->var_name = n.name;
		get->var_module = n.module;
		return get;
	};

	pin<Action> parse_type() {
		if (match("~"))
			return parse_expression();
		if (match("int"))
			return mk_const<ast::ConstInt64>(0);
		if (match("double"))
			return mk_const<ast::ConstDouble>(0.0);
		if (match("bool"))
			return make<ast::ConstBool>();
		if (match("void"))
			return make<ast::ConstVoid>();
		if (match("?")) {
			auto r = make<ast::If>();
			r->p[0] = make<ast::ConstBool>();
			r->p[1] = parse_type();
			return r;
		}
		auto parse_params = [&](pin<ast::MkLambda> fn) {
			if (!match(")")) {
				for (;;) {
					fn->names.push_back(make<ast::Var>());
					fn->names.back()->initializer = parse_type();
					if (match(")"))
						break;
					expect(",");
				}
			}
			return fn;
		};
		if (match("&")) {
			if (match("*")) {
				return fill(
					make<ast::MkWeakOp>(),
					fill(
						make<ast::ConformOp>(),
						mk_get("class or interface name")));
			}
			if (match("+")) {
				return fill(
					make<ast::MkWeakOp>(),
					fill(
						make<ast::FreezeOp>(),
						mk_get("class or interface name")));
			}
			if (match("(")) {
				auto fn = make<ast::ImmediateDelegate>();
				add_this_param(*fn, nullptr);  // to be set at type resolution pass
				parse_params(fn);
				fn->type_expression = parse_type();
				return fn;
			}
			return fill(make<ast::MkWeakOp>(), mk_get("class or interface name"));
		}
		if (match("+")) {
			return fill(make<ast::ConformOp>(), mk_get("class or interface name"));
		}
		if (match("*")) {
			return fill(make<ast::FreezeOp>(), mk_get("class or interface name"));
		}
		if (match("@")) {
			return mk_get("class or interface name");
		}
		if (match("fn")) {
			expect("(");
			auto fn = make<ast::Function>();
			parse_params(fn);
			fn->type_expression = parse_type();
			return fn;
		}
		if (match("(")) {
			auto fn = parse_params(make<ast::MkLambda>());
			fn->body.push_back(parse_type());
			return fn;
		}
		if (is_id_head(*cur)) {
			return fill(make<ast::RefOp>(), mk_get("class or interface name"));
		}
		error("Expected type name");
	}

	pin<Action> parse_statement() {
		auto r = parse_expression();
		if (auto as_get = dom::strict_cast<ast::Get>(r)) {
			if (!as_get->var && match("=")) {
				if (as_get->var_module)
					error("local var names should not contain '_'");
				auto block = make<ast::Block>();
				auto var = make_at_location<ast::Var>(*r);
				var->name = as_get->var_name;
				var->initializer = parse_expression();
				block->names.push_back(var);
				expect(";");
				parse_statement_sequence(block->body);
				return block;
			}
		}
		return r;
	}

	pin<Action> parse_expression() {
		return parse_elses();
	}

	pin<Action> parse_elses() {
		auto r = parse_ifs();
		while (match(":"))
			r = fill(make<ast::Else>(), r, parse_ifs());
		return r;
	}

	pin<Action> parse_ifs() {
		auto r = parse_ors();
		bool is_and = match("&&");
		if (is_and || match("?")) {
			auto rhs = make<ast::Block>();
			rhs->names.push_back(make<ast::Var>());
			rhs->names.back()->name = match("=") ? expect_id("local") : "_";
			rhs->body.push_back(parse_ifs());
			if (is_and)
				return fill(make<ast::LAnd>(), r, rhs);
			return fill(make<ast::If>(), r, rhs);
		}
		return r;
	}

	pin<Action> parse_ors() {
		auto r = parse_comparisons();
		while (match("||"))
			r = fill(make<ast::LOr>(), r, parse_comparisons());
		return r;
	}

	pin<Action> parse_comparisons() {
		auto r = parse_adds();
		if (match("==")) return fill(make<ast::EqOp>(), r, parse_adds());
		if (match(">=")) return fill(make<ast::NotOp>(), fill(make<ast::LtOp>(), r, parse_adds()));
		if (match("<=")) return fill(make<ast::NotOp>(), fill(make<ast::LtOp>(), parse_adds(), r));
		if (match("<")) return fill(make<ast::LtOp>(), r, parse_adds());
		if (match(">")) return fill(make<ast::LtOp>(), parse_adds(), r);
		if (match("!=")) return fill(make<ast::NotOp>(), fill(make<ast::EqOp>(), r, parse_adds()));
		return r;
	}

	pin<Action> parse_adds() {
		auto r = parse_muls();
		for (;;) {
			if (match("+")) r = fill(make<ast::AddOp>(), r, parse_muls());
			else if (match("-")) r = fill(make<ast::SubOp>(), r, parse_muls());
			else break;
		}
		return r;
	}

	pin<Action> parse_muls() {
		auto r = parse_unar();
		for (;;) {
			if (match("*")) r = fill(make<ast::MulOp>(), r, parse_unar());
			else if (match("/")) r = fill(make<ast::DivOp>(), r, parse_unar());
			else if (match("%")) r = fill(make<ast::ModOp>(), r, parse_unar());
			else if (match("<<")) r = fill(make<ast::ShlOp>(), r, parse_unar());
			else if (match(">>")) r = fill(make<ast::ShrOp>(), r, parse_unar());
			else if (match_and_not("&", '&')) r = fill(make<ast::AndOp>(), r, parse_unar());
			else if (match_and_not("|", '|')) r = fill(make<ast::OrOp>(), r, parse_unar());
			else if (match("^")) r = fill(make<ast::XorOp>(), r, parse_unar());
			else break;
		}
		return r;
	}

	pin<Action> parse_expression_in_parethesis() {
		expect("(");
		auto r = parse_expression();
		expect(")");
		return r;
	}

	pin<ast::BinaryOp> match_set_op() {
		if (match("+=")) return make<ast::AddOp>();
		if (match("-=")) return make<ast::SubOp>();
		if (match("*=")) return make<ast::MulOp>();
		if (match("/=")) return make<ast::DivOp>();
		if (match("%=")) return make<ast::ModOp>();
		if (match("<<=")) return make<ast::ShlOp>();
		if (match(">>=")) return make<ast::ShrOp>();
		if (match("&=")) return make<ast::AndOp>();
		if (match("|=")) return make<ast::OrOp>();
		if (match("^=")) return make<ast::XorOp>();
		return nullptr;
	}
	pin<Action> make_set_op(pin<Action> assignee, std::function<pin<Action>()> val) {
		if (auto as_get = dom::strict_cast<ast::Get>(assignee)) {
			auto set = make<ast::Set>();
			set->var = as_get->var;
			set->var_name = as_get->var_name;
			set->val = val();
			return set;
		}
		error("expected variable name in front of <set>= operator");
	}

	pin<Action> parse_unar() {
		auto r = parse_unar_head();
		for (;;) {
			if (match("(")) {
				auto call = make<ast::Call>();
				call->callee = r;
				r = call;
				while (!match(")")) {
					call->params.push_back(parse_expression());
					if (match(")"))
						break;
					expect(",");
				}
			} else if (match("[")) {
				auto gi = make<ast::GetAtIndex>();
				do
					gi->indexes.push_back(parse_expression());
				while (match(","));
				expect("]");
				if (auto op = match_set_op()) {
					auto block = make_at_location<ast::Block>(*gi);
					block->names.push_back(make_at_location<ast::Var>(*r));
					block->names.back()->initializer = r;
					auto indexed = make_at_location<ast::Get>(*gi);
					indexed->var = block->names.back();
					for (auto& var : gi->indexes) {
						block->names.push_back(make_at_location<ast::Var>(*var));
						block->names.back()->initializer = move(var);
						auto index = make_at_location<ast::Get>(*block->names.back()->initializer);
						index->var = block->names.back();
						var = index;
					}
					auto si = make_at_location<ast::SetAtIndex>(*gi);
					si->indexed = indexed;
					gi->indexed = indexed;
					si->indexes = gi->indexes;
					si->value = op;
					op->p[0] = gi;
					op->p[1] = parse_expression();
					block->body.push_back(si);
					r = block;
				} else {
					if (match(":=")) {
						auto si = make_at_location<ast::SetAtIndex>(*gi);
						si->indexes = move(gi->indexes);
						si->value = parse_expression();
						gi = si;
					}
					gi->indexed = r;
					r = gi;
				}
			} else if (match(".")) {
				if (match("&")) {
					auto d = make<ast::ImmediateDelegate>();
					d->base = r;
					d->name = expect_id("delegate name");
					auto& d_ref = delegates[d->name];
					if (d_ref)
						error("duplicated delegate name, ", d->name, " see ", *d_ref);
					d_ref = d;
					add_this_param(*d, nullptr);  // this type to be patched at the type resolver pass
					parse_fn_def(d);
					r = d;
				} else {
					pin<ast::FieldRef> gf = make<ast::GetField>();
					auto field_n = expect_long_name("field name", nullptr);
					gf->field_name = field_n.name;
					gf->field_module = field_n.module;
					if (auto op = match_set_op()) {
						auto block = make_at_location<ast::Block>(*gf);
						block->names.push_back(make_at_location<ast::Var>(*r));
						block->names.back()->initializer = r;
						auto field_base = make_at_location<ast::Get>(*gf);
						field_base->var = block->names.back();
						auto sf = make_at_location<ast::SetField>(*gf);
						sf->field_name = gf->field_name;
						sf->base = field_base;
						gf->base = field_base;
						sf->val = op;
						op->p[0] = gf;
						op->p[1] = parse_expression();
						block->body.push_back(sf);
						r = block;
					} else {
						if (match(":=")) {
							auto sf = make_at_location<ast::SetField>(*gf);
							sf->field_name = gf->field_name;
							sf->val = parse_expression();
							gf = sf;
						} else if (match("@=")) {
							auto sf = make_at_location<ast::SpliceField>(*gf);
							sf->field_name = gf->field_name;
							sf->val = parse_expression();
							gf = sf;
						}
						gf->base = r;
						r = gf;
					}
				}
			} else if (match(":=")) {
				r = make_set_op(r, [&] { return parse_expression(); });
			} else if (auto op = match_set_op()) {
				r = make_set_op(r, [&] {
					op->p[0] = r;
					op->p[1] = parse_expression();
					return op;
				});
			} else if (match("~")) {
				r = fill(make<ast::CastOp>(), r, parse_unar_head());
			} else
				return r;
		}
	}

	pin<Action> parse_unar_head() {
		if (match("(")) {
			pin<Action> start_expr;
			auto lambda = make<ast::MkLambda>();
			if (!match(")")) {
				start_expr = parse_expression();
				while (!match(")")) {
					expect(",");
					lambda->names.push_back(make<ast::Var>());
					lambda->names.back()->name = expect_id("parameter");
				}
			}
			if (match("{")) {
				if (start_expr) {
					if (auto as_ref = dom::strict_cast<ast::Get>(start_expr)) {
						lambda->names.insert(lambda->names.begin(), make<ast::Var>());
						lambda->names.front()->name = as_ref->var_name;
					} else {
						start_expr->error("lambda definition requires parameter name");
					}
				}
				parse_statement_sequence(lambda->body);
				expect("}");
				return lambda;
			} else if (lambda->names.empty() && start_expr){
				return start_expr;
			}
			lambda->error("expected single expression in parentesis or lambda {body}");
		}
		if (match("*"))
			return fill(make<ast::FreezeOp>(), parse_unar());
		if (match("@"))
			return fill(make<ast::CopyOp>(), parse_unar());
		if (match("&"))
			return fill(make<ast::MkWeakOp>(), parse_unar());
		if (match("!"))
			return fill(make<ast::NotOp>(), parse_unar());
		if (match("-"))
			return fill(make<ast::NegOp>(), parse_unar());
		if (match("~"))
			return fill(make<ast::XorOp>(),
				parse_unar(),
				mk_const<ast::ConstInt64>(-1));
		if (auto n = match_num()) {
			if (auto v = get_if<uint64_t>(&*n))
				return mk_const<ast::ConstInt64>(*v);
			if (auto v = get_if<double>(&*n))
				return mk_const<ast::ConstDouble>(*v);
		}
		if (match("{")) {
			auto r = make<ast::Block>();
			parse_statement_sequence(r->body);
			expect("}");
			return r;
		}
		bool matched_true = match("+");
		if (matched_true || match("?")) {
			auto r = make<ast::If>();
			auto cond = make<ast::ConstBool>();
			cond->value = matched_true;
			r->p[0] = cond;
			r->p[1] = parse_unar();
			return r;
		}
		matched_true = match("true");
		if (matched_true || match("false")) {
			auto r = make<ast::ConstBool>();
			r->value = matched_true;
			return r;
		}
		if (match("void"))
			return make<ast::ConstVoid>();
		if (match("int"))
			return fill(make<ast::ToIntOp>(), parse_expression_in_parethesis());
		if (match("double"))
			return fill(make<ast::ToFloatOp>(), parse_expression_in_parethesis());
		if (match("loop")) 
			return fill(make<ast::Loop>(), parse_unar());
		if (auto name = match("_")) {
			auto r = make<ast::Get>();
			r->var_name = "_";
			return r;
		}
		if (match_ns("'")) {
			auto r = make<ast::ConstInt64>();
			r->value = get_utf8(&cur);
			if (!r->value)
				error("incomplete character constant");
			expect("'");
			return r;
		}
		if (match_ns("\"")) {
			auto r = make<ast::ConstString>();
			for (;;) {
				auto prev_cur = cur;
				int c = get_utf8(&cur);
				pos += (int32_t) (cur - prev_cur);
				if (!c)
					error("incomplete string constant");
				if (c < ' ')
					error("control characters in the string constant");
				if (c == '"')
						break;
				if (c == '\\') {
					if (*cur == '\\') {
						c = '\\';
					} else if (*cur == '"') {
						c = '"';
					} else if (*cur == 'n') {
						c = '\n';
					} else if (*cur == 'r') {
						c = '\r';
					} else if (*cur == 't') {
						c = '\t';
					} else {
						c = 0;
						for (int d = get_digit(*cur); d < 16; cur++, pos++, d = get_digit(*cur))
							c = c * 16 + d;
						if (c == 0 || c > 0x10ffff)
							error("character code is outside the range 1..10ffff");
						if (*cur != '\\')
							error("expected closing '\\'");
					}
					cur++;
					pos++;
				}
				put_utf8(c, &r->value, [](void* ctx, int c) {
					*(string*)ctx += c;
					return 1;
				});
			}
			match_ws();
			return r;
		}
		if (is_id_head(*cur))
			return mk_get("name");
		error("syntax error");
	}

	template<typename T, typename VT>
	pin<Action> mk_const(VT&& v) {
		auto r = pin<T>::make();
		r->value = v;
		return r;
	}

	template<typename T>
	pin<T> make() {
		auto r = pin<T>::make();
		r->line = line;
		r->pos = pos;
		r->module = module;
		return r;
	}

	pin<ast::Action> fill(pin<ast::UnaryOp> op, pin<ast::Action> param) {
		op->p = param;
		return op;
	}

	pin<ast::Action> fill(pin<ast::BinaryOp> op, pin<ast::Action> param1, pin<ast::Action> param2) {
		op->p[0] = param1;
		op->p[1] = param2;
		return op;
	}

	optional<string> match_id() {
		if (!is_id_head(*cur))
			return nullopt;
		string result;
		while (is_id_body(*cur)) {
			result += *cur++;
			++pos;
		}
		match_ws();
		return result;
	}

	string expect_id(const char* message) {
		if (auto r = match_id())
			return *r;
		error("expected ", message);
	}

	variant<uint64_t, double> expect_number() {
		if (auto n = match_num())
			return *n;
		error("expected number");
	}

	int match_length(const char* str) { // returns 0 if not matched, length to skip if matched
		int i = 0;
		for (; str[i]; i++) {
			if (str[i] != cur[i])
				return 0;
		}
		return i;
	}

	bool match_ns(const char* str) {
		int i = match_length(str);
		if (i == 0 || (is_id_body(str[i - 1]) && is_id_body(cur[i])))
			return false;
		cur += i;
		pos += i;
		return true;
	}

	bool match(const char* str) {
		if (match_ns(str)) {
			match_ws();
			return true;
		}
		return false;
	}

	bool match_and_not(const char* str, char after) {
		if (int i = match_length(str)) {
			if (cur[i] != after) {
				cur += i;
				pos += i;
				match_ws();
				return true;
			}
		}
		return false;
	}

	void expect(const char* str) {
		if (!match(str))
			error(string("expected '") + str + "'");
	}

	bool match_ws() {
		const char* c = cur;
		for (;; line++, pos = 1) {
			while (*cur == ' ') {
				++cur;
				++pos;
			}
			if (*cur == '\t') {
				error("tabs aren't allowed as white space");
			}
			if (*cur == '/' && cur[1] == '/') {
				while (*cur && *cur != '\n' && *cur != '\r') {
					++cur;
				}
			}
			if (*cur == '\n') {
				if (*++cur == '\r')
					++cur;
			}
			else if (*cur == '\r') {
				if (*++cur == '\n')
					++cur;
			}
			else if (!*cur || *cur > ' ')
				return c != cur;
		}
	}

	static bool is_id_head(char c) {
		return
			(c >= 'a' && c <= 'z') ||
			(c >= 'A' && c <= 'Z');
	};

	static bool is_num(char c) {
		return c >= '0' && c <= '9';
	};

	static bool is_id_body(char c) {
		return is_id_head(c) || is_num(c);
	};

	static int get_digit(char c) {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return 255;
	}

	optional<variant<uint64_t, double>> match_num() {
		if (!is_num(*cur))
			return nullopt;
		int radix = 10;
		if (*cur == '0') {
			switch (cur[1]) {
			case 'x':
				radix = 16;
				cur += 2;
				break;
			case 'o':
				radix = 8;
				cur += 2;
				break;
			case 'b':
				radix = 2;
				cur += 2;
				break;
			default:
				break;
			}
		}
		uint64_t result = 0;
		for (;; cur++, pos++) {
			if (*cur == '_')
				continue;
			int digit = get_digit(*cur);
			if (digit == 255)
				break;
			if (digit >= radix)
				error("digit with value ", digit, " is not allowed in ", radix, "-base number");
			uint64_t next = result * radix + digit;
			if (next / radix != result)
				error("overflow");
			result = next;
		}
		if (*cur != '.' && *cur != 'e' && *cur != 'E') {
			match_ws();
			return result;
		}
		std::feclearexcept(FE_ALL_EXCEPT);
		double d = double(result);
		if (match_ns(".")) {
			for (double weight = 0.1; is_num(*cur); weight *= 0.1)
				d += weight * (*cur++ - '0');
		}
		if (match_ns("E") || match_ns("e")) {
			int sign = match_ns("-") ? -1 : (match_ns("+"), 1);
			int exp = 0;
			for (; *cur >= '0' && *cur < '9'; cur++)
				exp = exp * 10 + *cur - '0';
			d *= pow(10, exp * sign);
		}
		if (std::fetestexcept(FE_OVERFLOW | FE_UNDERFLOW))
			error("numeric overflow");
		match_ws();
		return d;
	}

	template<typename... T>
	[[noreturn]] void error(const T&... t) {
		std::cerr << "error " << ast::format_str(t...) << " " << module_name << ":" << line << ":" << pos << std::endl;
		throw 1;
	}

	bool is_eof() {
		return !*cur;
	}
};

}  // namespace

void parse(
	pin<Ast> ast,
	string start_module_name,
	module_text_provider_t module_text_provider)
{
	std::unordered_set<string> modules_in_dep_path;
	ast->starting_module = Parser(ast, start_module_name, modules_in_dep_path).parse(module_text_provider);
	if (!ast->starting_module->entry_point || ast->starting_module->entry_point->body.empty()) {
		std::cerr << "error starting module has no entry point" << std::endl;
		throw 1;
	}
}
