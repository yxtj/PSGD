#include "Master.h"
#include "network/NetworkThread.h"
#include "message/MType.h"
#include "logging/logging.h"
#include "util/Timer.h"
#include <future>
#include <numeric>

using namespace std;

Master::Master() : Runner() {
	typeDDeltaAny = MType::DDelta;
	typeDDeltaAll = 128 + MType::DDelta;
	factorDelta = 1.0;
	nx = 0;
	ny = 0;
	nPoint = 0;
	iter = 0;
	mtReportSum = 0.0;
	nReport = 0;
	mtUpdateSum = 0.0;
	nUpdate = 0;
	mtParameterSum = 0.0;
	mtOther = 0.0;
	timeOffset = 0.0;
	pie = nullptr;
	prs = nullptr;
	lastArchIter = 0;
	tmrArch.restart();
	doArchive = false;
	archDoing = false;
}

void Master::init(const ConfData* conf, const size_t lid)
{
	this->conf = conf;
	nWorker = conf->nw;
	globalBatchSize = conf->batchSize;
	localreportSize = conf->reportSize;
	nPointWorker.assign(nWorker, 0);
	trainer = TrainerFactory::generate(conf->optimizer, conf->optimizerParam);
	LOG_IF(trainer == nullptr, FATAL) << "Trainer is not set correctly";
	trainer->bindModel(&model);
	localID = lid;
	ln = conf->logIter;
	logName = "M";
	setLogThreadName(logName);
	initializeParameter();

	if(conf->mode == "bsp"){
		bspInit();
	} else if(conf->mode == "tap"){
		tapInit();
	} else if(conf->mode == "ssp"){
		sspInit();
	} else if(conf->mode == "sap"){
		sapInit();
	} else if(conf->mode == "fsp"){
		fspInit();
		pie =IntervalEstimatorFactory::generate(conf->intervalParam, nWorker, nPoint);
		LOG_IF(pie == nullptr, FATAL) << "Fail to initialize interval estimator with parameter: " << conf->intervalParam;
	} else if(conf->mode == "aap"){
		aapInit();
		prs = ReceiverSelectorFactory::generate(conf->mcastParam, nWorker);
		LOG_IF(prs == nullptr, FATAL) << "Fail to initialize receiver selector with parameter: " << conf->mcastParam;
	} else if(conf->mode == "pap"){
		papInit();
	}
}

void Master::run()
{
	registerHandlers();
	startMsgLoop(logName+"-MSG");
	
	LOG(INFO) << "Wait online messages";
	tmrTrain.restart();
	suOnline.wait();
	stat.t_data_load += tmrTrain.elapseSd();
	LOG(INFO) << "Send worker list";
	broadcastWorkerList();
	LOG(INFO)<<"Waiting dataset info to initialize parameters";
	try{
		checkDataset();
	} catch(exception& e){
		LOG(FATAL) << "Error in checking dataset: " << e.what();
	}
	LOG(INFO) << "Got x-length: " << nx << ", y-length: " << ny << ", data points: " << nPoint;
	LOG(INFO) << "Model parameter length: " << model.paramWidth();
	clearAccumulatedDelta();
	if(!conf->fnOutput.empty()){
		doArchive = true;
		archiver.init_write(conf->fnOutput, model.paramWidth(), conf->binary, conf->resume);
		LOG_IF(!archiver.valid(), FATAL) << "Cannot write to file: " << conf->fnOutput;
	}
	iter = 0;
	LOG(INFO) << "Coordinae initializing parameter";
	tmrTrain.restart();
	coordinateParameter();
	waitReady();
	stat.t_train_prepare += tmrTrain.elapseS();
	LOG(INFO) << "Start training";
	broadcastStart();

	tmrTrain.restart();
	archiveProgress(true);

	LOG(INFO)<<"Start traning with mode: "<<conf->mode;
	//tmrTrain.restart();
	iter = 1;
	if(conf->mode == "bsp"){
		bspProcess();
	} else if(conf->mode == "tap"){
		tapProcess();
	} else if(conf->mode == "ssp"){
		sspProcess();
	} else if(conf->mode == "sap"){
		sapProcess();
	} else if(conf->mode == "fsp"){
		fspProcess();
	} else if(conf->mode == "aap"){
		aapProcess();
	} else if(conf->mode == "pap"){
		papProcess();
	}
	--iter;
	double t = tmrTrain.elapseSd();
	LOG(INFO) << "Finish training. Time cost: " << t << ". Iterations: " << iter
		<< ". Average iteration time: " << t / iter;

	broadcastSignalTerminate();
	regDSPProcess(MType::DDelta, localCBBinder(&Master::handleDeltaTail));
	archiver.close();
	delete pie;
	delete prs;
	DLOG(INFO) << "un-send: " << net->pending_pkgs() << ", un-recv: " << net->unpicked_pkgs();
	finishStat();
	showStat();
	rph.deactivateType(MType::DDelta);
	suAllClosed.wait();
	stopMsgLoop();
}

Master::callback_t Master::localCBBinder(
	void (Master::*fp)(const std::string&, const RPCInfo&))
{
	return bind(fp, this, placeholders::_1, placeholders::_2);
}


void Master::registerHandlers()
{
	regDSPProcess(CType::NormalControl, localCBBinder(&Master::handleNormalControl));
	//regDSPProcess(MType::CReply, localCBBinder(&Master::handleReply));
	//regDSPProcess(MType::COnline, localCBBinder(&Master::handleOnline));
	//regDSPProcess(MType::CDataset, localCBBinder(&Master::handleDataset));

	regDSPImmediate(CType::ImmediateControl, localCBBinder(&Master::handleImmediateControl));
	//regDSPImmediate(MType::CClosed, localCBBinder(&Master::handleClosed));

	regDSPProcess(MType::DParameter, localCBBinder(&Master::handleParameter));
	// regDSPProcess(MType::DDelta, localCBBinder(&Master::handleDelta)); // for bsp and fsp
	// regDSPProcess(MType::DDelta, localCBBinder(&Master::handleDeltaTap)); // for tap

	addRPHEachSU(MType::COnline, suOnline);
	addRPHEachSU(MType::CWorkers, suWorker);
	addRPHEachSU(MType::CDataset, suDatasetInfo);
	addRPHEachSU(MType::CReady, suReady);
	addRPHEachSU(MType::DParameter, suParam);
	addRPHEachSU(MType::CTrainPause, suTPause);
	addRPHEachSU(MType::CTrainContinue, suTContinue);
	//addRPHEachSU(MType::CTerminate, suAllClosed); // in reply
	addRPHEachSU(MType::CClosed, suAllClosed); // in immediate handler
	addRPHAnySU(typeDDeltaAny, suDeltaAny);
	addRPHEachSU(typeDDeltaAll, suDeltaAll);
}

void Master::bindDataset(const DataHolder* pdh)
{
	trainer->bindDataset(pdh);
}

void Master::applyDelta(std::vector<double>& delta, const int source)
{
	Timer tmr;
	DVLOG(3) << "apply delta from " << source << " : " << delta
		<< "\nonto: " << model.getParameter().weights;
	model.accumulateParameter(delta, factorDelta);
	stat.n_point += bfDeltaDpCount;
	stat.t_par_calc += tmr.elapseSd();
}

void Master::clearAccumulatedDelta()
{
	bfDelta.assign(model.paramWidth(), 0.0);
	bfDeltaDpCount = 0;
}

void Master::accumulateDelta(const std::vector<double>& delta, const size_t cnt)
{
	Timer tmr;
	for(size_t i = 0; i < delta.size(); ++i)
		bfDelta[i] += delta[i];
	bfDeltaDpCount += cnt;
	stat.t_dlt_calc += tmr.elapseSd();
}

void Master::applyDeltaNext(const int d)
{
	if(d == 0)
		return;
	int size = min<int>(d, static_cast<int>(bfDelta.size()) - 1);
	for(int i = 1; i <= size; ++i){
		if(!bfDeltaNext[i].empty()){
			applyDelta(bfDeltaNext[i], -1);
			stat.n_point += bfDeltaDpCountNext[i];
		}
	}
}

void Master::clearAccumulatedDeltaNext(const int d)
{
	// move slot[d+1] to bfDelta
	// move slot[d+2,...] to slot[1,...]
	Timer tmr;
	size_t bfSize = bfDeltaNext.size();
	// set bfDelta
	if(bfSize <= d + 1){ // no slot d+1
		//VLOG(1) << "no slot " << d;
		clearAccumulatedDelta();
		bfDeltaNext.resize(1);
		bfDeltaDpCountNext.resize(1);
	} else{
		//VLOG(1) << "move " << d << " " << bfSize << " " << iter << " " << deltaIter;
		bfDelta = move(bfDeltaNext[d+1]);
		bfDeltaDpCount = bfDeltaDpCountNext[d+1];
	}
	// set bfDeltaNext
	size_t p = 1;
	for(size_t i = d + 2; i < bfSize; ++i){
		bfDeltaNext[p] = move(bfDeltaNext[i]);
		bfDeltaDpCountNext[p] = bfDeltaDpCountNext[i];
		++p;
	}
	if(bfSize >= d + 2){
		bfDeltaNext.erase(bfDeltaNext.begin() + (bfSize - d - 1), bfDeltaNext.end());
		bfDeltaDpCountNext.erase(bfDeltaDpCountNext.begin() + (bfSize - d - 1), bfDeltaDpCountNext.end());
	}
	stat.t_dlt_calc += tmr.elapseSd();
}

void Master::shiftAccumulatedDeltaNext()
{
	// move slot[1] to bfDelta
	// move slot[2,...] to slot[1,...]
	Timer tmr;
	size_t bfSize = bfDeltaNext.size();
	if(bfSize <= 1){
		clearAccumulatedDelta();
	} else {
		bfDelta = move(bfDeltaNext[1]);
		bfDeltaDpCount = bfDeltaDpCountNext[1];
	}
	for(size_t i = 2; i < bfSize; ++i){
		bfDeltaNext[i - 1] = move(bfDeltaNext[i]);
		bfDeltaDpCountNext[i - 1] = bfDeltaDpCountNext[i];
	}
	if(bfSize >= 2){
		bfDeltaNext.pop_back();
		bfDeltaDpCountNext.pop_back();
	}
	stat.t_dlt_calc += tmr.elapseSd();
}

void Master::accumulateDeltaNext(const int d, const std::vector<double>& delta, const size_t cnt)
{
	Timer tmr;
	if(bfDeltaNext.size() <= d){
		bfDeltaNext.resize(d + 1, vector<double>(delta.size(), 0.0));
		bfDeltaDpCountNext.resize(d + 1, 0);
	} else if(bfDeltaNext[d].empty()){
		bfDeltaNext[d].assign(delta.size(), 0.0);
	}
	for(size_t i = 0; i < delta.size(); ++i)
		bfDeltaNext[d][i] += delta[i];
	bfDeltaDpCountNext[d] += cnt;
	stat.t_dlt_calc += tmr.elapseSd();
}

bool Master::terminateCheck()
{
	return (iter > conf->tcIter)
		|| (tmrTrain.elapseSd() > conf->tcTime);
}

void Master::checkDataset()
{
	suDatasetInfo.wait_n_reset();
	model.checkData(nx, ny);
}

void Master::initializeParameter()
{
	model.init(conf->algorighm, conf->algParam);
	Parameter p;
	if(conf->resume){
		int i;
		double t;
		if(archiver.load_last(i, t, p)){
			iter = i;
			timeOffset = t;
			trainer->pm->setParameter(move(p));
		}
		LOG(INFO) << "Resume to iteration: " << i << ", at time: " << t;
		LOG_IF(model.paramWidth() != p.size(), FATAL) << "Size of resumed parameter does not match current model";
	} else{
		if(model.getKernel()->needInitParameterByData()){
			p.init(model.paramWidth(), 0.0);
		} else{
			p.init(model.paramWidth(), 0.01, 0.01, conf->seed);
		}
	}
	model.setParameter(move(p));
}

void Master::coordinateParameter()
{
	if(!conf->resume){
		if(model.getKernel()->needInitParameterByData()){
			suParam.wait_n_reset();
		}
	}
	broadcastParameter();
}

void Master::sendParameter(const int target)
{
	Timer tmr;
	DVLOG(3) << "send parameter to " << target << " with: " << model.getParameter().weights;
	net->send(wm.lid2nid(target), MType::DParameter, model.getParameter().weights);
	mtParameterSum += tmr.elapseSd();
	++stat.n_par_send;
}

void Master::broadcastParameter()
{
	Timer tmr;
	const auto& m = model.getParameter().weights;
	DVLOG(3) << "broadcast parameter: " << m;
	net->broadcast(MType::DParameter, m);
	mtParameterSum += tmr.elapseSd();
	stat.n_par_send += nWorker;
}

void Master::multicastParameter(const int source)
{
	Timer tmr;
	const auto& m = model.getParameter().weights;
	vector<int> targets = prs->getTargets(source);
	DVLOG(3) << "multicast parameter: " << m << " to " << targets;
	for(int& v : targets)
		v=wm.lid2nid(v);
	net->multicast(targets, MType::DParameter, m);
	mtParameterSum += tmr.elapseSd();
	stat.n_par_send += targets.size();
}

void Master::waitParameterConfirmed()
{
	suParam.wait_n_reset();
}

bool Master::needArchive()
{
	if(!doArchive)
		return false;
	if(iter - lastArchIter >= conf->arvIter
		|| tmrArch.elapseSd() >= conf->arvTime)
	{
		return true;
	}
	return false;
}

void Master::archiveProgress(const bool force)
{
	if(!force && !needArchive())
		return;
	if(archDoing)
		return;
	archDoing = true;
	lastArchIter = iter;
	tmrArch.restart();
	++stat.n_archive;
	std::async(launch::async, [&](int iter, double time, Parameter param){
		Timer t;
		archiver.dump(iter, time, param);
		archDoing = false;
		stat.t_archive += t.elapseSd();
	}, iter, timeOffset + tmrTrain.elapseSd(), ref(model.getParameter()));
}

size_t Master::estimateGlobalBatchSize()
{
	double mtu = mtUpdateSum / nUpdate;
	double mtb = mtParameterSum / stat.n_par_send;
	double mtr = mtReportSum / nReport;
	double mto = mtOther / iter;

	double wtd = accumulate(wtDatapoint.begin(), wtDatapoint.end(), 0.0) / wtDatapoint.size();
	double wtc = accumulate(wtDelta.begin(), wtDelta.end(), 0.0) / wtDelta.size();
	double wtr = accumulate(wtReport.begin(), wtReport.end(), 0.0) / wtReport.size();

	double up = nWorker * nWorker * (mtu + mtb) - nWorker * wtc;
	double down = wtd + (wtr - nWorker * mtr) / localreportSize;
	return static_cast<size_t>(up / down);
}

void Master::broadcastBatchSize(const size_t gbs)
{
	net->broadcast(CType::NormalControl, make_pair(MType::FGlobalBatchSize, gbs));
}

size_t Master::estimateLocalReportSize(const bool quick)
{
	double mtr = mtReportSum / nReport;
	double wtr = accumulate(wtReport.begin(), wtReport.end(), 0.0) / wtReport.size();
	double wtd = accumulate(wtDatapoint.begin(), wtDatapoint.end(), 0.0) / wtDatapoint.size();
	if(quick){
		return static_cast<size_t>((nWorker * mtr - wtr) / wtd);
	} else{
		double mtu = mtUpdateSum / nUpdate;
		double mtb = mtParameterSum / stat.n_par_send;
		double mto = mtOther / iter;
		double wtc = accumulate(wtDelta.begin(), wtDelta.end(), 0.0) / wtDelta.size();

		double up = globalBatchSize * wtr - nWorker * mtr;
		double down = nWorker * nWorker * (mtu + mtb) - nWorker * wtc - globalBatchSize * wtd;
		return static_cast<size_t>(up / down);
	}
}

void Master::broadcastReportSize(const size_t lrs)
{
	net->broadcast(CType::NormalControl, make_pair(MType::FLocalReportSize, lrs));
}

void Master::broadcastSizeConf(const size_t gbs, const size_t lrs)
{
	pair<size_t, size_t> data{ gbs, lrs };
	net->broadcast(CType::NormalControl, make_pair(MType::FSizeConf, data));
}

void Master::broadcastWorkerList()
{
	vector<pair<int, int>> temp = wm.list();
	net->broadcast(CType::NormalControl, make_pair(MType::CWorkers, temp));
	suWorker.wait();
}

void Master::waitReady()
{
	suReady.wait_n_reset();
}

void Master::broadcastStart()
{
	net->broadcast(CType::NormalControl, MType::CStart);
}

void Master::broadcastSignalPause()
{
	net->broadcast(CType::NormalControl, MType::CTrainPause);
	suTPause.wait_n_reset();
}

void Master::broadcastSignalContinue()
{
	net->broadcast(CType::NormalControl, MType::CTrainContinue);
	suTContinue.wait_n_reset();
}

void Master::broadcastSignalTerminate()
{
	net->broadcast(CType::ImmediateControl, MType::CTerminate);
}

void Master::waitDeltaFromAny(){
	suDeltaAny.wait_n_reset();
}

void Master::waitDeltaFromAll(){
	suDeltaAll.wait_n_reset();
}

void Master::gatherDelta()
{
	suDeltaAll.reset();
	net->broadcast(CType::NormalControl, MType::DRDelta);
	suDeltaAll.wait();
}

// handler - normal control

void Master::handleNormalControl(const std::string & data, const RPCInfo & info)
{
	int type = deserialize<int>(data);
	//const char* p = data.data() + sizeof(int);
	switch(type){
	case MType::CReply:
		handleReply(data.substr(sizeof(int)), info);
		break;
	case MType::COnline:
		handleOnline(data.substr(sizeof(int)), info);
		break;
	case MType::CDataset:
		handleDataset(data.substr(sizeof(int)), info);
		break;
	case MType::CReady:
		handleReady(data.substr(sizeof(int)), info);
		break;
		//MType::DDelta and MType::DReport are handled directly by message type
	}
}

void Master::handleReply(const std::string & data, const RPCInfo & info)
{
	Timer tmr;
	int type = deserialize<int>(data);
	stat.t_data_deserial += tmr.elapseSd();
	int source = wm.nid2lid(info.source);
	rph.input(type, source);
}

void Master::handleOnline(const std::string & data, const RPCInfo & info)
{
	Timer tmr;
	auto lid = deserialize<int>(data);
	stat.t_data_deserial += tmr.elapseSd();
	wm.registerID(info.source, lid);
	rph.input(MType::COnline, lid);
	sendReply(info, MType::COnline);
}

void Master::handleDataset(const std::string& data, const RPCInfo& info){
	Timer tmr;
	size_t tnx, tny, tnp;
	tie(tnx, tny, tnp) = deserialize<tuple<size_t, size_t, size_t>>(data);
	stat.t_data_deserial += tmr.elapseSd();
	int source = wm.nid2lid(info.source);
	bool flag = false;
	if(nx == 0){
		nx = tnx;
	} else if(nx != tnx){
		flag = true;
	}
	if(ny == 0){
		ny = tny;
	} else if(ny != tny){
		flag = true;
	}
	LOG_IF(flag, FATAL) << "dataset on " << source << " does not match with others."
		<< " X-match: " << (nx == tnx) << ", Y-match: " << (ny == tny);
	nPointWorker[source] = tnp;
	nPoint += tnp;
	rph.input(MType::CDataset, source);
	sendReply(info, MType::CDataset);
}

void Master::handleReady(const std::string & data, const RPCInfo & info)
{
	int src = wm.nid2lid(info.source);
	rph.input(MType::CReady, src);
}

// handler - immediate control

void Master::handleImmediateControl(const std::string & data, const RPCInfo & info)
{
	int type = deserialize<int>(data);
	//const char* p = data.data() + sizeof(int);
	switch(type){
	case MType::CClosed:
		handleClosed(data.substr(sizeof(int)), info);
		break;
	}
}

void Master::handleClosed(const std::string & data, const RPCInfo & info)
{
	int source = wm.nid2lid(info.source);
	rph.input(MType::CClosed, source);
}

// handler - data

void Master::handleParameter(const std::string & data, const RPCInfo & info)
{
	Timer tmr;
	vector<double> param = deserialize<vector<double>>(data);
	stat.t_data_deserial += tmr.elapseSd();
	tmr.restart();
	int s = wm.nid2lid(info.source);
	model.accumulateParameter(param);
	++stat.n_dlt_recv;
	stat.t_par_calc += tmr.elapseSd();
	rph.input(MType::DParameter, s);
}

void Master::handleReport(const std::string& data, const RPCInfo& info)
{
	Timer tmr;
	vector<double> report = deserialize<vector<double>>(data);
	stat.t_data_deserial += tmr.elapseSd();
	int wid = wm.nid2lid(info.source);
	bool flag = false;
	// format: #-processed-data-points, time-per-data-point, time-per-delta-sending, time-per-report-sending
	{
		lock_guard<mutex> lg(mReportProc);
		int t = reportProcEach[wid];
		int cnt = static_cast<int>(report[0]);
		reportProcEach[wid] = cnt;
		reportProcTotal += cnt - t;
		if(reportProcTotal > conf->batchSize)
			flag = true;
	}
	if(conf->papSearchBatchSize || conf->papSearchReportFreq){
		wtDatapoint[wid] = report[1];
		wtDelta[wid] = report[2];
		wtReport[wid] = report[3];
	}
	if(flag)
		suPap.notify();
	mtReportSum += tmr.elapseSd();
}

void Master::handleDeltaTail(const std::string & data, const RPCInfo & info)
{
	Timer tmr;
	auto deltaMsg = deserialize<pair<size_t, vector<double>>>(data);
	stat.t_data_deserial += tmr.elapseSd();
	int s = wm.nid2lid(info.source);
	applyDelta(deltaMsg.second, s);
	++stat.n_dlt_recv;
}
