/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Author: Yixi Chen <yixi.chen@kaust.edu.sa>
 */

#include <stdio.h>
#include <string.h>
#include "tensorflow/c/c_api.h"
#include "offrac.h"

void NoOpDeallocator(void* data, size_t a, void* b) {}
// Comparison function for qsort (descending order)
int compare_desc(const void *a, const void *b) {
    return (*(uint32_t *)b - *(uint32_t *)a);
}

// return the buf size after topk, ideally should be k
// return -1 if error
int topk(void* out_buf , int req_size, void* in_buf) {

    int k = 16;
    int size = req_size/sizeof(uint32_t);
    // Check if k is valid
    uint32_t *buf_u32 = (uint32_t *)out_buf;
    if (k <= 0 || k > size) {
        fprintf(stderr, "Invalid value of k: %d\n", k);
        return -1;
    }

    memcpy(out_buf, in_buf, req_size);

    qsort(buf_u32, size, sizeof(uint32_t), compare_desc);

    for (int i = k; i < size; i++) {
      buf_u32[i] = 0;
    }

    return k;
}

int norm(void* out_buf , int req_size, void* in_buf) {
    float *float_buf = (float *)out_buf;

    // Find the min and max values
    float min_val = FLT_MAX;
    float max_val = -FLT_MAX;

    int size = req_size/sizeof(uint32_t);

    for (int i = 0; i < size; i++) {
        if (float_buf[i] < min_val) {
            min_val = float_buf[i];
        }
        if (float_buf[i] > max_val) {
            max_val = float_buf[i];
        }
    }

    // Check if min and max values are the same
    if (min_val == max_val) {
        //fprintf(stderr, "All elements are the same\n");
        max_val += min_val;
        //return -1;
    }

    // Normalize the elements using min-max normalization
    for (int i = 0; i < size; i++) {
        float_buf[i] = (float_buf[i] - min_val) / (max_val - min_val);
    }

    return size;
}

int logit(void* out_buf , int req_size, void* in_buf) {
    float *float_buf = (float *)out_buf;

    int size = req_size/sizeof(uint32_t);

    // Apply the logistic function to each element
    for (int i = 0; i < size; i++) {
        float_buf[i] = 1.0f / (1.0f + expf(-float_buf[i]));
    }

    return size;
}

int cnn(void* out_buf, int req_size, void* in_buf){
    // Load the TensorFlow model (SavedModel format)
    TF_Graph* graph = TF_NewGraph();
    TF_Status* status = TF_NewStatus();

    // Define session options and load the SavedModel
    TF_SessionOptions* session_opts = TF_NewSessionOptions();
    TF_Buffer* run_options = NULL;
    const char* tags = "serve";
    TF_Session* session = TF_LoadSessionFromSavedModel(session_opts, run_options, "tf/saved_model", &tags, 1, graph, NULL, status);
    // Prepare input tensor dimensions for a single image (not batch)
    int64_t input_dims[] = {1, 64, 64, 3};  // (1 image, 64x64, RGB)

    // Get input and output tensor names
    TF_Output input_op = {TF_GraphOperationByName(graph, "serving_default_input_1"), 0};
    TF_Output output_op = {TF_GraphOperationByName(graph, "StatefulPartitionedCall"), 0};

    if (input_op.oper == NULL || output_op.oper == NULL) {
        fprintf(stderr, "Failed to get input/output tensor\n");
        return -1;
    }

    float* input_data = (float*) malloc(IMAGE_SIZE*sizeof(float));
    if (input_data == NULL) {
        fprintf(stderr, "Failed to allocate input data\n");
        return -1;
    }

    // Copy one image from the buffer
    uint8_t* input_bytes = (uint8_t*)in_buf;
    for (int i = 0; i < IMAGE_SIZE; i++) {
	  input_data[i] = (float)input_bytes[i];
    }

    // Create an input tensor for this single image
    TF_Tensor* input_tensor = TF_NewTensor(TF_FLOAT, input_dims, 4, input_data, IMAGE_SIZE * sizeof(float), &NoOpDeallocator, NULL);

    // Run inference
    TF_Tensor* output_tensor = NULL;
    TF_SessionRun(session, NULL, &input_op, &input_tensor, 1, &output_op, &output_tensor, 1, NULL, 0, NULL, status);

    // Retrieve and print the result for this image
    void* buff = TF_TensorData(output_tensor);
    float* offsets = (float*)buff;

    /*
      printf("Image %d result:\n", i);
      for (int j = 0; j < 10; j++) {
      printf("%f\n", offsets[j]);
      }
    */
    // Store the result back in buf (in-place update)
    // TODO: update to output
    memcpy(out_buf, offsets, 10 * sizeof(float));

    // Clean up for this image
    TF_DeleteTensor(input_tensor);
    TF_DeleteTensor(output_tensor);


    memcpy(out_buf, buff, 10 * sizeof(float));
    // Clean up global resources
    TF_DeleteSession(session, status);
    TF_DeleteSessionOptions(session_opts);
    TF_DeleteGraph(graph);
    TF_DeleteStatus(status);

    return 10 * sizeof(float);
}
