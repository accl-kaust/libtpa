# How to run tf

# Quick start

## Install tensorflow
  Follow this instruction [here](https://www.tensorflow.org/install/lang_c) 
  > I am using v2.18, remember to update the command path during installation for the corresponding version

## Compile
    ```sh
    gcc -o test_tf test_tf.c -I$HOME/test/keras2onnx_env/lib/python3.8/site-packages/tensorflow/include -L/usr/local/lib -ltensorflow -pthread -ldl -lm
    ```

## Run
    ```
    ./test_tf
    ```
