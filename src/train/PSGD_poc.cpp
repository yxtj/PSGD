#include "PSGD_poc.h"
#include "impl/TopKHolder.hpp"
#include <algorithm>
#include <random>
#include <numeric>

using namespace std;

void PSGD_poc::init(const std::vector<std::string>& param)
{
	try{
		rate = stod(param[0]);
		mergeDim = param.size() > 1 ? param[1] == "1" : true;
	} catch(exception& e){
		throw invalid_argument("Cannot parse parameters for GD\n" + string(e.what()));
	}
}

std::string PSGD_poc::name() const
{
	return "psgd_poc";
}

void PSGD_poc::ready()
{
	// initialize parameter by data
	paramWidth = pm->paramWidth();
	if(pm->getKernel()->needInitParameterByData()){
		Parameter p;
		p.init(paramWidth, 0.0);
		pm->setParameter(p);
		size_t s = pd->size();
		for(size_t i = 0; i < s; ++i){
			pm->getKernel()->initVariables(
				pd->get(i).x, pm->getParameter().weights, pd->get(i).y, nullptr);
		}
	}
}

std::pair<size_t, std::vector<double>> PSGD_poc::batchDelta(
	const size_t start, const size_t cnt, const bool avg)
{
	if(mergeDim){
		return batchDelta_point(start, cnt, avg);
	} else{
		return batchDelta_dim(start, cnt, avg);
	}
}

std::pair<size_t, std::vector<double>> PSGD_poc::batchDelta(
	std::atomic<bool>& cond, const size_t start, const size_t cnt, const bool avg)
{
	return batchDelta(start, cnt, avg);
}

std::pair<size_t, std::vector<double>> PSGD_poc::batchDelta_point(
	const size_t start, const size_t cnt, const bool avg)
{
	vector<double> grad(paramWidth, 0.0);
	// calculate all gradient and priority
	vector<vector<double>> gradient_buffer;
	gradient_buffer.reserve(pd->size());
	vector<pair<float, int>> priority_record;
	priority_record.reserve(pd->size());
	for(size_t i = 0; i < pd->size(); ++i){
		auto g = pm->gradient(pd->get(i));
		auto p = inner_product(g.begin(), g.end(), g.begin(), 1.0);
		gradient_buffer.push_back(move(g));
		priority_record.emplace_back(static_cast<float>(p), static_cast<int>(i));
	}
	auto it_mid = cnt >= pd->size() ? priority_record.end() : priority_record.begin() + cnt;
	partial_sort(priority_record.begin(), it_mid, priority_record.end(),
		[](const pair<float, int>& l, const pair<float, int>& r){
		return l.first < r.first;
	});
	priority_record.erase(it_mid, priority_record.end());
	// calculate gradient
	for(auto& pc : priority_record){
		auto& g = gradient_buffer[pc.second];
		for(size_t i = 0; i < paramWidth; ++i)
			grad[i] += g[i];
	}
	double factor = -rate;
	if(avg)
		factor /= cnt;
	for(auto& v : grad)
		v *= factor;
	return make_pair(cnt, move(grad));
}

std::pair<size_t, std::vector<double>> PSGD_poc::batchDelta_dim(
	const size_t start, const size_t cnt, const bool avg)
{
	vector<double> grad(paramWidth, 0.0);
	size_t nblock = min(cnt, pd->size())*paramWidth;
	// calculate all gradient and priority
	vector<vector<double>> gradient_buffer;
	gradient_buffer.reserve(pd->size());
	TopKHolder<pair<int, int>> tpk(nblock);
	vector<pair<float, int>> priority_record;
	priority_record.reserve(pd->size());
	for(size_t i = 0; i < pd->size(); ++i){
		auto g = pm->gradient(pd->get(i));
		gradient_buffer.push_back(move(g));
		for(size_t j = 0; j < paramWidth; ++j){
			tpk.update(make_pair((int)i, (int)j), g[j]);
		}
	}
	// calculate gradient
	vector<int> dimCnt(paramWidth, 0);
	for(auto& cp : tpk.data){
		auto c = cp.first;
		auto& g = gradient_buffer[c.first];
		int i = c.second;
		++dimCnt[i];
		grad[i] += g[i];
	}
	double factor = -rate;
	if(!avg)
		factor *= cnt;
	for(size_t i = 0; i < paramWidth; ++i){
		auto& v = grad[i];
		v *= factor / dimCnt[i];
	}
	return make_pair(cnt, move(grad));
}
