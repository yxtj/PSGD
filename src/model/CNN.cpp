#include "CNN.h"
#include "util/Util.h"
#include <stdexcept>
#include <regex>
using namespace std;

// -------- CNN --------

void CNN::init(const int xlength, const std::string & param)
{
	initBasic(xlength, param);
    // example: 10x10,4c3x3,relu,max2x2,f
	// example: 10-4:c:3-1:relu-1:max:2-1:f
    // format: <n>:<type>[:<shape>]
    //     shape of convolutional node: <k1>*<k2>
    //     shape of fully-connected node: none
    try{
		string pm = preprocessParam(param);
		net.init(param);
		net.bindGradLossFunc(&CNN::gradLoss);
    }catch(exception& e){
		throw invalid_argument(string("Unable to create network: ") + e.what());
    }
	// check input layer size
	if(xlength != net.lenFeatureLayer[0])
		throw invalid_argument("The dataset does not match the input layer of the network");
	// check FC layer
	for(size_t i = 0; i < net.nLayer; ++i){
		if(i != net.nLayer - 1 && net.typeLayer[i] == NodeType::FC){
			throw invalid_argument("Only the last layer can be a FC layer.");
		} else if(i == net.nLayer - 1 && net.typeLayer[i] != NodeType::FC){
			throw invalid_argument("The last layer must be a FC layer.");
		}
	}
}

std::string CNN::name() const{
	return "cnn";
}

bool CNN::dataNeedConstant() const{
	return false;
}

int CNN::lengthParameter() const
{
	return net.lengthParameter();
}

std::vector<double> CNN::predict(
	const std::vector<double>& x, const std::vector<double>& w) const
{
	return net.predict(x, w);
}

int CNN::classify(const double p) const
{
	return p >= 0.5 ? 1 : 0;
}

double CNN::loss(const std::vector<double>& pred, const std::vector<double>& label) const {
	double res = 0.0;
	for(size_t i = 0; i < pred.size(); ++i){
		double t = pred[i] - label[i];
		res += t * t;
	}
	//return 0.5 * res;
	return res;
}

std::vector<double> CNN::gradLoss(const std::vector<double>& pred, const std::vector<double>& label)
{
	vector<double> res(pred.size());
	for(size_t i = 0; i < pred.size(); ++i){
		res[i] = pred[i] - label[i];
	}
	return res;
}

std::vector<double> CNN::gradient(
	const std::vector<double>& x, const std::vector<double>& w, const std::vector<double>& y) const
{
	return net.gradient(x, w, y);
}

std::string CNN::preprocessParam(const std::string & param)
{
	string sr("(\\d+(?*[\\*x]\\d+)*)");
	// 3c5p4 -> 3:c:5,sig,max:4
	regex runit("(\\d+)c" + sr + "([srt])?p" + sr);

	string res;
	auto first = param.begin();
	auto last = param.end();
	std::smatch sm;
	while(regex_search(first, last, sm, runit)) {
		first = sm[0].second; // the last matched position
		//cout << sm[0] << " - " << sm[1] << " - " << sm[2] << " - " << sm[3] << " - " << sm[4] << "\n";
		string act;
		if(!sm[3].matched || sm[3] == "s"){
			act = "sigmoid";
		} else if(sm[3] == "r"){
			act = "relu";
		} else if(sm[3] == "t"){
			act = "tanh";
		} else{
			act = sm[3].str();
		}
		res += sm.prefix().str() + sm[1].str() + ":c:" + sm[2].str() + "-" + act + "-max:" + sm[4].str();
	}
	res += string(first, last);

	return res;
}
