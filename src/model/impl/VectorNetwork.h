#pragma once
#include "NodeBase.h"
#include <string>
#include <vector>
#include <tuple>

struct VectorNetwork {
	using feature_t = std::vector<double>;

	// structure info
	int nLayer;
	std::vector<int> nNodeLayer;
	std::vector<NodeType> typeLayer;

	// node info
	std::vector<std::vector<int>> shapeNode; // the shape parameter of the node in each layer

	// feature info
	// the output at layer i is a matrix with shape (nFeatureLayer[i]*lenFeatureLayer[i])
	std::vector<int> numFeatureLayer; // the number of feature of layer i
	std::vector<int> lenFeatureLayer; // the length of a feature at layer i (=shpFeatureLayer[i].size())
	std::vector<std::vector<int>> shpFeatureLayer; // the actual shape of each feature at layer i

	// weight info
	std::vector<int> nWeightNode; // # of weight for a node at layer i
	std::vector<int> weightOffsetLayer; // weight offset of the the first node at layer i

	std::vector<std::vector<NodeBase*>> nodes;

public:
	void init(const std::string& param); // calls parse and build
	// parse the parameter string into structure info
	std::vector<std::tuple<int, NodeTypeGeneral, std::string>> parse(const std::string& param);
	// use the input structure info to build up the network
	void build(const std::vector<std::tuple<int, NodeTypeGeneral, std::string>>& structure);
	int lengthParameter() const;

	std::vector<double> predict(const std::vector<double>& x, const std::vector<double>& w) const;
	std::vector<double> gradient(
		const std::vector<double>& x, const std::vector<double>& w, const std::vector<double>& dL_dy);

	~VectorNetwork();

private:
	std::vector<int> getShape(const std::string& str);
	int getSize(const std::vector<int>& shape);
	int getSize(const std::string& str);

	void createLayerInput(const size_t i, const std::vector<int>& shape);
	void createLayerAct(const size_t i, const std::string& type);
	void createLayerSum(const size_t i, const int n);
	void createLayerConv(const size_t i, const int n, const std::vector<int>& shape);
	void createLayerPool(const size_t i, const std::string& type, const std::vector<int>& shape);
	void createLayerRecr(const size_t i, const int n, const std::vector<int>& oshape);
	void createLayerFC(const size_t i, const int n);

	// set all data members and generate all nodes for layer i
	// precondition: weightOffsetLayer[i]
	void coreCreateLayer(const size_t i, const NodeType type, const int n, const std::vector<int>& shape);
	// precondition: typeLayer[i], nNodeLayer[i], shapeNode[i], weightOffsetLayer[i]
	// postcondition: nWeightNode[i], nodes[i], weightOffsetLayer[i+1]
	void createNodesForLayer(const size_t i);
};
