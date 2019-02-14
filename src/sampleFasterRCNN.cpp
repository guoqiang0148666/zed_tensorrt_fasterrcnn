#include <cassert>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <sys/stat.h>
#include <cmath>
#include <time.h>
#include <cuda_runtime_api.h>
#include <cudnn.h>
#include <cublas_v2.h>
#include <memory>
#include <cstring>
#include <algorithm>

#include "NvCaffeParser.h"
#include "NvInferPlugin.h"
#include "common.h"
#include "factoryFasterRCNN.h"

static Logger gLogger;

#include <fstream>
#include <iostream>
#include <cstdlib>
using namespace std;

#include <opencv2/dnn.hpp>
#include <opencv2/dnn/shape_utils.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>

#include <vector>

#include <sl_zed/Camera.hpp>

using namespace sl;

cv::Mat slMat2cvMat(Mat& input);
void printHelp();

// using namespace nvinfer1;
// using namespace nvcaffeparser1;
// using namespace plugin;

// using namespace cv;
// using namespace cv::dnn;

//for opencv cmd line param tool
const char* params
    = "{ help           | false | print usage         }"
      "{ proto          |       | model configuration }"
      "{ model          |       | model weights       }"
      "{ camera_device  | 0     | camera device number}"
      "{ video          |       | video or image for detection}"
      "{ min_confidence | 0.5   | min confidence      }";


// stuff we know about the network and the caffe input/output blobs
static const int INPUT_C = 3;
static const int INPUT_H = 375;
static const int INPUT_W = 500;
static const int IM_INFO_SIZE = 3;
static const int OUTPUT_CLS_SIZE = 2;
static const int OUTPUT_BBOX_SIZE = OUTPUT_CLS_SIZE * 4;

//const std::string CLASSES[OUTPUT_CLS_SIZE]{ "background", "aeroplane", "bicycle", "bird", "boat", "bottle", "bus", "car", "cat", "chair", "cow", "diningtable", "dog", "horse", "motorbike", "person", "pottedplant", "sheep", "sofa", "train", "tvmonitor" };
const std::string CLASSES[OUTPUT_CLS_SIZE]{ "background", "traffic_cone"};
//const std::string CLASSES[OUTPUT_CLS_SIZE]{ "background", "tree"};

const char* INPUT_BLOB_NAME0 = "data";
const char* INPUT_BLOB_NAME1 = "im_info";
const char* OUTPUT_BLOB_NAME0 = "bbox_pred";
const char* OUTPUT_BLOB_NAME1 = "cls_prob";
const char* OUTPUT_BLOB_NAME2 = "rois";

struct PPM
{
	std::string magic, fileName;
	int h, w, max;
	uint8_t buffer[INPUT_C*INPUT_H*INPUT_W];
};

struct BBox
{
	float x1, y1, x2, y2;
};

std::string locateFile(const std::string& input)
{
    std::vector<std::string> dirs{"data/samples/faster-rcnn/", "data/faster-rcnn/"};
    return locateFile(input, dirs);
}

// simple PPM (portable pixel map) reader
// void readPPMFile(const std::string& filename, PPM& ppm)
// {
// 	ppm.fileName = filename;
// 	std::ifstream infile(locateFile(filename), std::ifstream::binary);
// 	infile >> ppm.magic >> ppm.w >> ppm.h >> ppm.max;
// 	infile.seekg(1, infile.cur);
// 	infile.read(reinterpret_cast<char*>(ppm.buffer), ppm.w * ppm.h * 3);
// }

// void writePPMFileWithBBox(const std::string& filename, PPM& ppm, const BBox& bbox)
// {
// 	std::ofstream outfile("./" + filename, std::ofstream::binary);
// 	assert(!outfile.fail());
// 	outfile << "P6" << "\n" << ppm.w << " " << ppm.h << "\n" << ppm.max << "\n";
// 	auto round = [](float x)->int {return int(std::floor(x + 0.5f)); };
// 	for (int x = int(bbox.x1); x < int(bbox.x2); ++x)
// 	{
// 		// bbox top border
// 		ppm.buffer[(round(bbox.y1) * ppm.w + x) * 3] = 255;
// 		ppm.buffer[(round(bbox.y1) * ppm.w + x) * 3 + 1] = 0;
// 		ppm.buffer[(round(bbox.y1) * ppm.w + x) * 3 + 2] = 0;
// 		// bbox bottom border
// 		ppm.buffer[(round(bbox.y2) * ppm.w + x) * 3] = 255;
// 		ppm.buffer[(round(bbox.y2) * ppm.w + x) * 3 + 1] = 0;
// 		ppm.buffer[(round(bbox.y2) * ppm.w + x) * 3 + 2] = 0;
// 	}
// 	for (int y = int(bbox.y1); y < int(bbox.y2); ++y)
// 	{
// 		// bbox left border
// 		ppm.buffer[(y * ppm.w + round(bbox.x1)) * 3] = 255;
// 		ppm.buffer[(y * ppm.w + round(bbox.x1)) * 3 + 1] = 0;
// 		ppm.buffer[(y * ppm.w + round(bbox.x1)) * 3 + 2] = 0;
// 		// bbox right border
// 		ppm.buffer[(y * ppm.w + round(bbox.x2)) * 3] = 255;
// 		ppm.buffer[(y * ppm.w + round(bbox.x2)) * 3 + 1] = 0;
// 		ppm.buffer[(y * ppm.w + round(bbox.x2)) * 3 + 2] = 0;
// 	}
// 	outfile.write(reinterpret_cast<char*>(ppm.buffer), ppm.w * ppm.h * 3);
// }

void caffeToTRTModel(const std::string& deployFile,			// name for caffe prototxt
	const std::string& modelFile,			// name for model
	const std::vector<std::string>& outputs,		// network outputs
	unsigned int maxBatchSize,				// batch size - NB must be at least as large as the batch we want to run with)
	nvcaffeparser1::IPluginFactory* pluginFactory,	// factory for plugin layers
	IHostMemory **trtModelStream)			// output stream for the TensorRT model
{
	// create the builder
	IBuilder* builder = createInferBuilder(gLogger);

	// parse the caffe model to populate the network, then set the outputs
	INetworkDefinition* network = builder->createNetwork();
	ICaffeParser* parser = createCaffeParser();
	parser->setPluginFactory(pluginFactory);

	std::cout << "Begin parsing model..." << std::endl;
	const IBlobNameToTensor* blobNameToTensor = parser->parse(locateFile(deployFile).c_str(),
		locateFile(modelFile).c_str(),
		*network,
		DataType::kFLOAT);
	std::cout << "End parsing model..." << std::endl;
	// specify which tensors are outputs
	for (auto& s : outputs)
		network->markOutput(*blobNameToTensor->find(s.c_str()));

	// Build the engine
	builder->setMaxBatchSize(maxBatchSize);
	builder->setMaxWorkspaceSize(10 << 20);	// we need about 6MB of scratch space for the plugin layer for batch size 5

	std::cout << "Begin building engine..." << std::endl;
	ICudaEngine* engine = builder->buildCudaEngine(*network);
	assert(engine);
	std::cout << "End building engine..." << std::endl;

	// we don't need the network any more, and we can destroy the parser
	network->destroy();
	parser->destroy();

	// serialize the engine, then close everything down
	(*trtModelStream) = engine->serialize();

	engine->destroy();
	builder->destroy();
	shutdownProtobufLibrary();
}

void doInference(IExecutionContext& context, float* inputData, float* inputImInfo, float* outputBboxPred, float* outputClsProb, float *outputRois, int batchSize)
{
	const ICudaEngine& engine = context.getEngine();
	// input and output buffer pointers that we pass to the engine - the engine requires exactly IEngine::getNbBindings(),
	// of these, but in this case we know that there is exactly 2 inputs and 3 outputs.
	assert(engine.getNbBindings() == 5);
	void* buffers[5];

	// In order to bind the buffers, we need to know the names of the input and output tensors.
	// note that indices are guaranteed to be less than IEngine::getNbBindings()
	int inputIndex0 = engine.getBindingIndex(INPUT_BLOB_NAME0),
		inputIndex1 = engine.getBindingIndex(INPUT_BLOB_NAME1),
		outputIndex0 = engine.getBindingIndex(OUTPUT_BLOB_NAME0),
		outputIndex1 = engine.getBindingIndex(OUTPUT_BLOB_NAME1),
		outputIndex2 = engine.getBindingIndex(OUTPUT_BLOB_NAME2);


	// create GPU buffers and a stream
	CHECK(cudaMalloc(&buffers[inputIndex0], batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float)));   // data
	CHECK(cudaMalloc(&buffers[inputIndex1], batchSize * IM_INFO_SIZE * sizeof(float)));                  // im_info
	CHECK(cudaMalloc(&buffers[outputIndex0], batchSize * nmsMaxOut * OUTPUT_BBOX_SIZE * sizeof(float))); // bbox_pred
	CHECK(cudaMalloc(&buffers[outputIndex1], batchSize * nmsMaxOut * OUTPUT_CLS_SIZE * sizeof(float)));  // cls_prob
	CHECK(cudaMalloc(&buffers[outputIndex2], batchSize * nmsMaxOut * 4 * sizeof(float)));                // rois

	cudaStream_t stream;
	CHECK(cudaStreamCreate(&stream));

	// DMA the input to the GPU,  execute the batch asynchronously, and DMA it back:
	CHECK(cudaMemcpyAsync(buffers[inputIndex0], inputData, batchSize * INPUT_C * INPUT_H * INPUT_W * sizeof(float), cudaMemcpyHostToDevice, stream));
	CHECK(cudaMemcpyAsync(buffers[inputIndex1], inputImInfo, batchSize * IM_INFO_SIZE * sizeof(float), cudaMemcpyHostToDevice, stream));
	context.enqueue(batchSize, buffers, stream, nullptr);
	CHECK(cudaMemcpyAsync(outputBboxPred, buffers[outputIndex0], batchSize * nmsMaxOut * OUTPUT_BBOX_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
	CHECK(cudaMemcpyAsync(outputClsProb, buffers[outputIndex1], batchSize * nmsMaxOut * OUTPUT_CLS_SIZE * sizeof(float), cudaMemcpyDeviceToHost, stream));
	CHECK(cudaMemcpyAsync(outputRois, buffers[outputIndex2], batchSize * nmsMaxOut * 4 * sizeof(float), cudaMemcpyDeviceToHost, stream));
	cudaStreamSynchronize(stream);


	// release the stream and the buffers
	cudaStreamDestroy(stream);
	CHECK(cudaFree(buffers[inputIndex0]));
	CHECK(cudaFree(buffers[inputIndex1]));
	CHECK(cudaFree(buffers[outputIndex0]));
	CHECK(cudaFree(buffers[outputIndex1]));
	CHECK(cudaFree(buffers[outputIndex2]));
}

void bboxTransformInvAndClip(float* rois, float* deltas, float* predBBoxes, float* imInfo,
	const int N, const int nmsMaxOut, const int numCls)
{
	float width, height, ctr_x, ctr_y;
	float dx, dy, dw, dh, pred_ctr_x, pred_ctr_y, pred_w, pred_h;
	float *deltas_offset, *predBBoxes_offset, *imInfo_offset;
	for (int i = 0; i < N * nmsMaxOut; ++i)
	{
		width = rois[i * 4 + 2] - rois[i * 4] + 1;
		height = rois[i * 4 + 3] - rois[i * 4 + 1] + 1;
		ctr_x = rois[i * 4] + 0.5f * width;
		ctr_y = rois[i * 4 + 1] + 0.5f * height;
		deltas_offset = deltas + i * numCls * 4;
		predBBoxes_offset = predBBoxes + i * numCls * 4;
		imInfo_offset = imInfo + i / nmsMaxOut * 3;
		for (int j = 0; j < numCls; ++j)
		{
			dx = deltas_offset[j * 4];
			dy = deltas_offset[j * 4 + 1];
			dw = deltas_offset[j * 4 + 2];
			dh = deltas_offset[j * 4 + 3];
			pred_ctr_x = dx * width + ctr_x;
			pred_ctr_y = dy * height + ctr_y;
			pred_w = exp(dw) * width;
			pred_h = exp(dh) * height;
			predBBoxes_offset[j * 4] = std::max(std::min(pred_ctr_x - 0.5f * pred_w, imInfo_offset[1] - 1.f), 0.f);
			predBBoxes_offset[j * 4 + 1] = std::max(std::min(pred_ctr_y - 0.5f * pred_h, imInfo_offset[0] - 1.f), 0.f);
			predBBoxes_offset[j * 4 + 2] = std::max(std::min(pred_ctr_x + 0.5f * pred_w, imInfo_offset[1] - 1.f), 0.f);
			predBBoxes_offset[j * 4 + 3] = std::max(std::min(pred_ctr_y + 0.5f * pred_h, imInfo_offset[0] - 1.f), 0.f);
		}
	}
}

std::vector<int> nms(std::vector<std::pair<float, int> >& score_index, float* bbox, const int classNum, const int numClasses, const float nms_threshold)
{
	auto overlap1D = [](float x1min, float x1max, float x2min, float x2max) -> float {
		if (x1min > x2min) {
			std::swap(x1min, x2min);
			std::swap(x1max, x2max);
		}
		return x1max < x2min ? 0 : std::min(x1max, x2max) - x2min;
	};
	auto computeIoU = [&overlap1D](float* bbox1, float* bbox2) -> float {
		float overlapX = overlap1D(bbox1[0], bbox1[2], bbox2[0], bbox2[2]);
		float overlapY = overlap1D(bbox1[1], bbox1[3], bbox2[1], bbox2[3]);
		float area1 = (bbox1[2] - bbox1[0]) * (bbox1[3] - bbox1[1]);
		float area2 = (bbox2[2] - bbox2[0]) * (bbox2[3] - bbox2[1]);
		float overlap2D = overlapX * overlapY;
		float u = area1 + area2 - overlap2D;
		return u == 0 ? 0 : overlap2D / u;
	};

	std::vector<int> indices;
	for (auto i : score_index)
	{
		const int idx = i.second;
		bool keep = true;
		for (unsigned k = 0; k < indices.size(); ++k)
		{
			if (keep)
			{
				const int kept_idx = indices[k];
				float overlap = computeIoU(&bbox[(idx*numClasses + classNum) * 4],
					&bbox[(kept_idx*numClasses + classNum) * 4]);
				keep = overlap <= nms_threshold;
			}
			else
				break;
		}
		if (keep) indices.push_back(idx);
	}
	return indices;
}


int main(int argc, char** argv)
{
	cv::CommandLineParser parser(argc, argv, params);
    	
    // cv::VideoCapture cap;
    // if (parser.get<cv::String>("video").empty())
    // {
    //     //int cameraDevice = parser.get<int>("camera_device");
    //     cap = cv::VideoCapture("nvcamerasrc ! video/x-raw(memory:NVMM), width=(int)640, height=(int)480,format=(string)I420, framerate=(fraction)30/1 ! nvvidconv ! video/x-raw, format=(string)BGRx ! videoconvert ! video/x-raw, format=(string)BGR ! appsink");
    //     if(!cap.isOpened())
    //     {
    //         cout << "Couldn't find camera: " << endl;
    //         return -1;
    //     }
    // }
    // else
    // {
    //     cap.open(parser.get<cv::String>("video"));
    //     if(!cap.isOpened())
    //     {
    //         cout << "Couldn't open image or video: " << parser.get<cv::String>("video") << endl;
    //         return -1;
    //     }
    // }

	sl::Camera zed;
    sl::InitParameters init_params;
    init_params.camera_resolution = sl::RESOLUTION_HD720;
    init_params.coordinate_units = sl::UNIT_METER;
	if (argc > 1) init_params.svo_input_filename.set(argv[1]);

	// Open the camera
    ERROR_CODE err = zed.open(init_params);
    if (err != SUCCESS) {
        printf("%s\n", toString(err).c_str());
        zed.close();
        return 1; // Quit if an error occurred
    }

	 // Set runtime parameters after opening the camera
    RuntimeParameters runtime_parameters;
    runtime_parameters.sensing_mode = SENSING_MODE_STANDARD;

    // Prepare new image size to retrieve half-resolution images
    Resolution image_size = zed.getResolution();
    // int new_width = image_size.width / 2;
    // int new_height = image_size.height / 2;

    // To share data between sl::Mat and cv::Mat, use slMat2cvMat()
    // Only the headers and pointer to the sl::Mat are copied, not the data itself
    Mat image_zed(image_size.width, image_size.height, MAT_TYPE_8U_C4);
    cv::Mat image_ocv = slMat2cvMat(image_zed);

	// create a TensorRT model from the caffe model and serialize it to a stream
	FRCNNPluginFactory pluginFactorySerialize;
	IHostMemory *trtModelStream{ nullptr };
	// batch size
	const int N = 1;
	// caffeToTRTModel("faster_rcnn_test_iplugin.prototxt",
	// 	"almonds_vgg16_faster_rcnn_iter_20000.caffemodel",
	// 	std::vector < std::string > { OUTPUT_BLOB_NAME0, OUTPUT_BLOB_NAME1, OUTPUT_BLOB_NAME2 },
	// 	N, &pluginFactorySerialize, &trtModelStream);
	caffeToTRTModel("faster_rcnn_test_iplugin.prototxt",
		"vgg16_faster_rcnn_iter_20000.caffemodel",
		std::vector < std::string > { OUTPUT_BLOB_NAME0, OUTPUT_BLOB_NAME1, OUTPUT_BLOB_NAME2 },
		N, &pluginFactorySerialize, &trtModelStream);
    assert(trtModelStream != nullptr);
	pluginFactorySerialize.destroyPlugin();

/*
	// read a random sample image
	srand(unsigned(time(nullptr)));
	// available images
	std::vector<std::string> imageList = { "IMG_1551.ppm", "IMG_1553.ppm", "IMG_1556.ppm", "IMG_1559.ppm" };
	//std::vector<std::string> imageList = { "IMG_1559.ppm" };
	std::vector<PPM> ppms(N);

	
	std::random_shuffle(imageList.begin(), imageList.end(), [](int i) {return rand() % i; });
	assert(ppms.size() <= imageList.size());
*/

	float imInfo[N * 3]; // input im_info
	for (int i = 0; i < N; ++i)
	{
		// readPPMFile(imageList[i], ppms[i]);
		// imInfo[i * 3] = float(ppms[i].h);   // number of rows
		// imInfo[i * 3 + 1] = float(ppms[i].w); // number of columns
		// imInfo[i * 3 + 2] = 1;         // image scale
		imInfo[i * 3] 	  = float(INPUT_H);	//float(ppms[i].h); 	// number of rows
		imInfo[i * 3 + 1] = float(INPUT_W);	//float(ppms[i].w); 	// number of columns
		imInfo[i * 3 + 2] = 1;         		// image scale
	}

	float* data = new float[N*INPUT_C*INPUT_H*INPUT_W];
	// pixel mean used by the Faster R-CNN's author
	float pixelMean[3]{ 102.9801f, 115.9465f, 122.7717f };  // also in BGR order

	// deserialize the engine
	IRuntime* runtime = createInferRuntime(gLogger);
    assert(runtime != nullptr);
	FRCNNPluginFactory pluginFactory;
	ICudaEngine* engine = runtime->deserializeCudaEngine(trtModelStream->data(), trtModelStream->size(), &pluginFactory);
    assert(engine != nullptr);
    trtModelStream->destroy();
	IExecutionContext *context = engine->createExecutionContext();
    assert(context != nullptr);

	// host memory for outputs
	float* rois = new float[N * nmsMaxOut * 4];
	float* bboxPreds = new float[N * nmsMaxOut * OUTPUT_BBOX_SIZE];
	float* clsProbs = new float[N * nmsMaxOut * OUTPUT_CLS_SIZE];

	// predicted bounding boxes
	float* predBBoxes = new float[N * nmsMaxOut * OUTPUT_BBOX_SIZE];

	float img_scale_ratio_w, img_scale_ratio_h;

	
	
    char key = ' ';
    while (key != 'q')
    //for (int m=0; m<1; m++)
    {
        if (zed.grab(runtime_parameters) == SUCCESS) {
			// std::cout << "zed.grab - success" << std::endl;
			zed.retrieveImage(image_zed, VIEW_LEFT, MEM_CPU, image_size.width, image_size.height);
		
        //cap >> frame; // get a new frame from camera/video or read image

        if (image_ocv.empty())
        {
            cv::waitKey(1);
            break;
        }
		
		// std::cout << "image channels: " << image_ocv.channels() << std::endl;
		// if (image_ocv.channels() == 4) {
		// 	std::cout << "Converting Color" << std::endl;	
		// 	cv::cvtColor(image_ocv, image_ocv, cv::COLOR_BGRA2BGR);
		// }
		// std::cout << "OpenCV captured img size[ : " << image_ocv.size().height <<"x"<<image_ocv.size().width<<"]"<<std::endl; 

		// scale to size required by engine - INPUT_H*INPUT_W
		cv::Size size(INPUT_W,INPUT_H);	//the dst image size,e.g.100x100
		cv::Mat image_ocv_scaled;	
		cv::resize(image_ocv,image_ocv_scaled,size);	//resize image from src->dst
		// std::cout << "OpenCV scaled img size[ : " << image_ocv_scaled.size().height <<"x"<<image_ocv_scaled.size().width<<"]"<<std::endl;	

		img_scale_ratio_w=float(image_ocv.size().width)/float(image_ocv_scaled.size().width);	//beaware the int/int rounding 
		img_scale_ratio_h=float(image_ocv.size().height)/float(image_ocv_scaled.size().height);
		cv::Mat_<cv::Vec3f>::iterator it;
		unsigned volChl = INPUT_H*INPUT_W;
		for (int c = 0; c < INPUT_C; ++c)                              
		{
			cv::Mat_<cv::Vec3b>::iterator it = image_ocv_scaled.begin<cv::Vec3b>();	//cv::Vec3f not working - reason still unknown...
			// the color image to input should be in BGR order
			for (unsigned j = 0; j < volChl; ++j)
			{
				//OpenCV read in image_ocv as BGR format, by default, thus need only deduct the mean value
				data[c*volChl + j] = float((*it)[c]) - pixelMean[c];
				it++;
			}
		}
	// run inference
	doInference(*context, data, imInfo, bboxPreds, clsProbs, rois, N);

	// unscale back to raw image space
	for (int i = 0; i < N; ++i)
	{
		float * rois_offset = rois + i * nmsMaxOut * 4;
		for (int j = 0; j < nmsMaxOut * 4 && imInfo[i * 3 + 2] != 1; ++j)
			rois_offset[j] /= imInfo[i * 3 + 2];
	}
	bboxTransformInvAndClip(rois, bboxPreds, predBBoxes, imInfo, N, nmsMaxOut, OUTPUT_CLS_SIZE);

	const float nms_threshold = 0.3f;
	const float score_threshold = 0.8f;

	for (int i = 0; i < N; ++i)
	{
		float *bbox = predBBoxes + i * nmsMaxOut * OUTPUT_BBOX_SIZE;
		float *scores = clsProbs + i * nmsMaxOut * OUTPUT_CLS_SIZE;
		for (int c = 1; c < OUTPUT_CLS_SIZE; ++c) // skip the background
		{
			std::vector<std::pair<float, int> > score_index;
			for (int r = 0; r < nmsMaxOut; ++r)
			{
				if (scores[r*OUTPUT_CLS_SIZE + c] > score_threshold)
				{
					score_index.push_back(std::make_pair(scores[r*OUTPUT_CLS_SIZE + c], r));
					std::stable_sort(score_index.begin(), score_index.end(),
						[](const std::pair<float, int>& pair1,
							const std::pair<float, int>& pair2) 
					{
						return pair1.first > pair2.first;
					});
				}
			}
			// apply NMS algorithm
			std::vector<int> indices = nms(score_index, bbox, c, OUTPUT_CLS_SIZE, nms_threshold);

			// Show results
			// if (indices.size()>0)
			// 	std::cout<<"size of index: "<<indices.size()<<std::endl;

			for (unsigned k = 0; k < indices.size(); ++k)
			{
				int idx = indices[k];
				// std::string storeName = CLASSES[c] + "-" + std::to_string(scores[idx*OUTPUT_CLS_SIZE + c]) + ".ppm";
				// std::cout << "Detected " << CLASSES[c] << " in " << ppms[i].fileName << " with confidence " << scores[idx*OUTPUT_CLS_SIZE + c] * 100.0f << "% "
				// 	<< " (Result stored in " << storeName << ")." << std::endl;

				//BBox b{ bbox[idx*OUTPUT_BBOX_SIZE + c * 4], bbox[idx*OUTPUT_BBOX_SIZE + c * 4 + 1], bbox[idx*OUTPUT_BBOX_SIZE + c * 4 + 2], bbox[idx*OUTPUT_BBOX_SIZE + c * 4 + 3] };
				//writePPMFileWithBBox(storeName, ppms[i], b);

				int xLeftBottom = bbox[idx*OUTPUT_BBOX_SIZE + c * 4];
				int yLeftBottom = bbox[idx*OUTPUT_BBOX_SIZE + c * 4 + 1];
				int xRightTop 	= bbox[idx*OUTPUT_BBOX_SIZE + c * 4 + 2];
				int yRightTop	= bbox[idx*OUTPUT_BBOX_SIZE + c * 4 + 3];

				//cv::Rect object0((int)xLeftBottom, (int)yLeftBottom, (int)(xRightTop - xLeftBottom), (int)(yRightTop - yLeftBottom));
	                	//rectangle(image_ocv_scaled, object0, cv::Scalar(0, 255, 0));				
	                									
				//convert vertexes back into unscaled image
				xLeftBottom = (int) (xLeftBottom * img_scale_ratio_w); 
				yLeftBottom = (int) (yLeftBottom * img_scale_ratio_h);
				xRightTop   = (int) (xRightTop * img_scale_ratio_w); 
				yRightTop   = (int) (yRightTop * img_scale_ratio_h);
				
				//draw bbox
				cv::Rect object((int)xLeftBottom, (int)yLeftBottom, (int)(xRightTop - xLeftBottom), (int)(yRightTop - yLeftBottom));
	                	rectangle(image_ocv, object, cv::Scalar(0, 255, 0));
	                	
	                	//draw cls tags
	                	ostringstream ss("");
	                	//ss.str("");
                		ss << scores[idx*OUTPUT_CLS_SIZE + c] * 100.0f;
		                cv::String str_conf(ss.str());
                		cv::String label = cv::String(CLASSES[c]) + ": " + str_conf;
                		int baseLine = 0;
		                cv::Size labelSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
                		cv::rectangle(image_ocv, cv::Rect(cv::Point(xLeftBottom, yLeftBottom - labelSize.height), cv::Size(labelSize.width, labelSize.height + baseLine)),
                          	cv::Scalar(255,128,255), CV_FILLED);
                		cv::putText(image_ocv, label, cv::Point(xLeftBottom, yLeftBottom), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0,0,0));				
			}

		}
		}
	}

	//update result in opencv window
        cv::imshow("detections1", image_ocv);
        //cv::imshow("detections2", image_ocv_scaled);
        //if (cv::waitKey(3) >= 0) break;	//1ms
		key = cv::waitKey(10);
    }

	zed.close();

	// destroy the engine
	context->destroy();
	engine->destroy();
	runtime->destroy();

	// Destroy plugins created by factory
	pluginFactory.destroyPlugin();
	
	delete[] data;
	delete[] rois;
	delete[] bboxPreds;
	delete[] clsProbs;
	delete[] predBBoxes;

	return 0;
}

/**
* Conversion function between sl::Mat and cv::Mat
**/
cv::Mat slMat2cvMat(Mat& input) {
    // Mapping between MAT_TYPE and CV_TYPE
    int cv_type = -1;
    switch (input.getDataType()) {
        case MAT_TYPE_32F_C1: cv_type = CV_32FC1; break;
        case MAT_TYPE_32F_C2: cv_type = CV_32FC2; break;
        case MAT_TYPE_32F_C3: cv_type = CV_32FC3; break;
        case MAT_TYPE_32F_C4: cv_type = CV_32FC4; break;
        case MAT_TYPE_8U_C1: cv_type = CV_8UC1; break;
        case MAT_TYPE_8U_C2: cv_type = CV_8UC2; break;
        case MAT_TYPE_8U_C3: cv_type = CV_8UC3; break;
        case MAT_TYPE_8U_C4: cv_type = CV_8UC4; break;
        default: break;
    }

    // Since cv::Mat data requires a uchar* pointer, we get the uchar1 pointer from sl::Mat (getPtr<T>())
    // cv::Mat and sl::Mat will share a single memory structure
    return cv::Mat(input.getHeight(), input.getWidth(), cv_type, input.getPtr<sl::uchar1>(MEM_CPU));
}

/**
* This function displays help in console
**/
void printHelp() {
    std::cout << " Press 's' to save Side by side images" << std::endl;
    std::cout << " Press 'p' to save Point Cloud" << std::endl;
    std::cout << " Press 'd' to save Depth image" << std::endl;
    std::cout << " Press 'm' to switch Point Cloud format" << std::endl;
    std::cout << " Press 'n' to switch Depth format" << std::endl;
}
