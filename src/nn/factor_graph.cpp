#include "nn/factor_graph.h"
#include "nn/variable.h"

namespace gnn
{

FactorGraph::FactorGraph()
{
	var_dict.clear();
	factor_dict.clear();

	isReady.clear();
	isRequired.clear();

	factorEdges.clear();
	varEdges.clear();

	ready_dict.clear();
	var_list.clear();
	factor_list.clear();
}

size_t FactorGraph::VarIdx(std::string var_name)
{
	ASSERT(var_dict.count(var_name), "variable " << var_name << " is not registered");
	return var_dict[var_name].first;
}

size_t FactorGraph::VarIdx(FactorGraph::VarPtr var)
{
	return VarIdx(var->name);
}

size_t FactorGraph::FacIdx(std::shared_ptr<Factor> fac)
{
	ASSERT(factor_dict.count(fac->name), "factor " << fac->name << " is not registered");
	return factor_dict[fac->name].first;
}

void FactorGraph::DependencyParse(std::initializer_list<FactorGraph::VarPtr> targets)
{
	isRequired.resize(var_dict.size());
	for (size_t i = 0; i < isRequired.size(); ++i)
		isRequired[i] = false;

	while (!q.empty())
		q.pop();

	for (auto p : targets)
	{
		isRequired[VarIdx(p)] = true;
		q.push(p->name);
	}

	while (!q.empty())
	{
		auto cur_var = q.front();
		q.pop();
		auto& in_list = varEdges[cur_var].first;
		for (auto factor : in_list)
		{
			auto& in_vars = factorEdges[factor->name].first;
			for (auto p : in_vars)
				if (!isRequired[VarIdx(p)])
				{
					isRequired[VarIdx(p)] = true;
					q.push(p->name);	
				}
		}
	}
}

void FactorGraph::SequentialForward(std::initializer_list< FactorGraph::VarPtr > targets, 
									std::map<std::string, void*> feed_dict)
{
	n_pending.resize(factor_list.size());

	while (!q.empty())
		q.pop();

	for (size_t i = 0; i < factor_list.size(); ++i)
	{		
		auto& in_list = factorEdges[factor_list[i]->name].first;
		n_pending[i] = in_list.size();
	}

	for (size_t i = 0; i < isReady.size(); ++i)
		if (isReady[i])
		{
			auto& out_list = varEdges[var_list[i]->name].second;	
			for (auto f : out_list)
			{
				auto& n_rest = n_pending[FacIdx(f)];
				if (--n_rest == 0)
				{
					q.push(f->name);
				}
			}
		}

	while (!q.empty())
	{
		auto& cur_name = q.front();
		q.pop();

		auto& factor = factor_dict[cur_name].second;
		auto& operands = factorEdges[cur_name].first;
		auto& outputs = factorEdges[cur_name].second;

		bool necessary = false;
		for (auto p : outputs)
			if (isRequired[VarIdx(p)])
			{
				necessary = true;
				break;
			}
		if (!necessary)
			continue;

		factor->Forward(operands, outputs);
		for (auto p : outputs)
		{
			isReady[VarIdx(p)] = true;
			auto& out_list = varEdges[p->name].second;

			for (auto f : out_list)
			{
				auto& n_rest = n_pending[FacIdx(f)];
				if (--n_rest == 0)
				{
					q.push(f->name);
				}
			}
		}
	}
}

FactorGraph::VarList FactorGraph::FeedForward(std::initializer_list<FactorGraph::VarPtr> targets, 
											std::map<std::string, void*> feed_dict,
											uint n_thread)
{	
	DependencyParse(targets);
	isReady.resize(var_dict.size());
	for (size_t i = 0; i < isReady.size(); ++i)
		isReady[i] = false;	

	for (auto st : ready_dict)
		isReady[VarIdx(st.first)] = true;

	for (auto p : feed_dict)
	{
		isReady[VarIdx(p.first)] = true;
		var_dict[p.first].second->SetRef(p.second);
	}

	if (n_thread == 1)
		SequentialForward(targets, feed_dict);
	else {
		throw std::runtime_error("not implemented");	
	}

	for (auto p : targets)
	{
		ASSERT(isReady[VarIdx(p)], "required variable " << p->name << " is not ready");
	}

	std::vector<FactorGraph::VarPtr> result;
	for (auto p : targets)
		result.push_back(p);

	return result;
}

void FactorGraph::SequentialBackward(std::initializer_list< FactorGraph::VarPtr > targets)
{
	for (size_t i = 0; i < factor_list.size(); ++i)
	{		
		auto& out_list = factorEdges[factor_list[i]->name].second;
		n_pending[i] =0;
		for (auto name : out_list)
			if (isRequired[VarIdx(name)])
				n_pending[i]++;
	}
	while (!q.empty())
		q.pop();
	for (auto& p : targets)
	{
		auto& in_list = varEdges[p->name].first;

		for (auto& f : in_list)
		{
			auto& n_rest = n_pending[FacIdx(f)];
			if (--n_rest == 0)
				q.push(f->name);
		}
	}
	while (!q.empty())
	{
		auto& cur_name = q.front();
		std::cerr << cur_name << std::endl;
		q.pop();

		auto& factor = factor_dict[cur_name].second;
		auto& operands = factorEdges[cur_name].first;
		auto& outputs = factorEdges[cur_name].second;

		bool necessary = factor->properr == PropErr::T;
		if (necessary)
		{
			bool ok = false;
			for (auto p : operands)
				if (!p->IsConst())
				{
					ok = true; break;
				}
			necessary = ok;
		}
		
		if (necessary)
			factor->Backward(operands, outputs);

		for (auto p : operands)
		{
			auto& in_list = varEdges[p->name].first;
			for (auto f : in_list)
			{
				auto& n_rest = n_pending[FacIdx(f)];
				if (--n_rest == 0)
					q.push(f->name);
			}
		}		
	}

}

void FactorGraph::BackPropagate(std::initializer_list< FactorGraph::VarPtr > targets, 
								uint n_thread)
{
	ASSERT(isReady.size() == var_dict.size() && n_pending.size() == factor_list.size() && isReady.size() == isRequired.size(), 
		"unexpected change of computation graph in backward stage");
	for (size_t i = 0; i < var_list.size(); ++i)
		if (!var_list[i]->IsConst())
		{
			auto* diff_var = dynamic_cast<IDifferentiable*>(var_list[i].get());
			diff_var->ZeroGrad();
		}

	for (auto p : targets)
	{
		ASSERT(varEdges[p->name].second.size() == 0, "only allow backprop from top variables");
		ASSERT(!p->IsConst(), "cannot calc grad for const variable");
		auto* diff_var = dynamic_cast<IDifferentiable*>(p.get());
		diff_var->OnesGrad();
	}

	if (n_thread == 1)
		SequentialBackward(targets);
	else {
		throw std::runtime_error("not implemented");	
	}
}

void FactorGraph::AddVar(VarPtr var, bool need_feed)
{
	ASSERT(var_dict.count(var->name) == 0 && varEdges.count(var->name) == 0, 
			fmt::sprintf("variable %s is already inserted", var->name.c_str()));
	varEdges[var->name] = std::pair<FactorList, FactorList>();
	var_dict[var->name] = std::make_pair(var_list.size(), var);
	var_list.push_back(var);

	if (!need_feed)
		ready_dict[var->name] = var;
}

}