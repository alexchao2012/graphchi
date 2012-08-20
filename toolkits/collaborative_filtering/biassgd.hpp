

/**
 * @file
 * @author  Danny Bickson
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2012] [Aapo Kyrola, Guy Blelloch, Carlos Guestrin / Carnegie Mellon University]
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 * http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 
 *
 * @section DESCRIPTION
 *
 * Common code for BIASSGD implementations.
 */



#ifndef DEF_BIASSGDHPP
#define DEF_BIASSGDHPP

#include <assert.h>
#include <cmath>
#include <errno.h>
#include <string>
#include <stdint.h>

#include "../../example_apps/matrix_factorization/matrixmarket/mmio.h"
#include "../../example_apps/matrix_factorization/matrixmarket/mmio.c"

#include "api/chifilenames.hpp"
#include "api/vertex_aggregator.hpp"
#include "preprocessing/sharder.hpp"

#include "eigen_wrapper.hpp"
using namespace graphchi;

#ifndef NLATENT
#define NLATENT 20   // Dimension of the latent factors. You can specify this in compile time as well (in make).
#endif

double biassgd_lambda = 1e-3;
double biassgd_gamma = 1e-3;
double biassgd_step_dec = 0.9;
double minval = -1e100;
double maxval = 1e100;
std::string training;
std::string validation;
std::string test;
int M, N;


/// RMSE computation
double rmse=0.0;
double globalMean = 0;

// Hackish: we need to count the number of left
// and right vertices in the bipartite graph ourselves.
vid_t max_left_vertex =0 ;
vid_t max_right_vertex = 0;

struct vertex_data {
    double d[NLATENT];
    double rmse;
    double bias;
 
    vertex_data() {
    }
    
    void init() {
        for(int k=0; k < NLATENT; k++) 
           d[k] =  drand48(); 
        rmse = 0;
        bias = 0;
    }
    
    double & operator[] (int idx) {
        return d[idx];
    }
    bool operator!=(const vertex_data &oth) const {
        for(int i=0; i<NLATENT; i++) { if (d[i] != oth.d[i]) return true; }
        return false;
    }
    
    double dot(vertex_data &oth) const {
        double x=0;
        for(int i=0; i<NLATENT; i++) x+= oth.d[i]*d[i];
        return x;
    }
    
};



struct biassgd_factor_and_weight {
    vertex_data factor;
    float weight;
    
    biassgd_factor_and_weight() {}
    
    biassgd_factor_and_weight(float obs) {
        weight = obs;
        factor.init();
    }
};


 
 /**
 * Create a bipartite graph from a matrix. Each row corresponds to vertex
 * with the same id as the row number (0-based), but vertices correponsing to columns
 * have id + num-rows.
 */
template <typename biassgd_edge_type>
int convert_matrixmarket_for_BIASSGD(std::string base_filename) {
    // Note, code based on: http://math.nist.gov/MatrixMarket/mmio/c/example_read.c
    int ret_code;
    MM_typecode matcode;
    FILE *f;
    int nz;   
    
    /**
     * Create sharder object
     */
    int nshards;
    if ((nshards = find_shards<biassgd_edge_type>(base_filename, get_option_string("nshards", "auto")))) {
        logstream(LOG_INFO) << "File " << base_filename << " was already preprocessed, won't do it again. " << std::endl;
        logstream(LOG_INFO) << "If this is not intended, please delete the shard files and try again. " << std::endl;
        return nshards;
    }   
    
    sharder<biassgd_edge_type> sharderobj(base_filename);
    sharderobj.start_preprocessing();
    
    
    if ((f = fopen(base_filename.c_str(), "r")) == NULL) {
        logstream(LOG_FATAL) << "Could not open file: " << base_filename << ", error: " << strerror(errno) << std::endl;
    }
    
    
    if (mm_read_banner(f, &matcode) != 0)
        logstream(LOG_FATAL) << "Could not process Matrix Market banner. File: " << base_filename << std::endl;
    
    /*  This is how one can screen matrix types if their application */
    /*  only supports a subset of the Matrix Market data types.      */
    
    if (mm_is_complex(matcode) || !mm_is_sparse(matcode))
        logstream(LOG_FATAL) << "Sorry, this application does not support complex values and requires a sparse matrix." << std::endl;
    
    /* find out size of sparse matrix .... */
    
    if ((ret_code = mm_read_mtx_crd_size(f, &M, &N, &nz)) !=0) {
        logstream(LOG_FATAL) << "Failed reading matrix size: error=" << ret_code << std::endl;
    }
    
    
    logstream(LOG_INFO) << "Starting to read matrix-market input. Matrix dimensions: " 
    << M << " x " << N << ", non-zeros: " << nz << std::endl;
    
    
    if (!sharderobj.preprocessed_file_exists()) {
        for (int i=0; i<nz; i++)
        {
            int I, J;
            double val;
            int rc = fscanf(f, "%d %d %lg\n", &I, &J, &val);
            if (rc != 3)
              logstream(LOG_FATAL)<<"Error processing input file " << base_filename << " at data row " << i <<std::endl;
            I--;  /* adjust from 1-based to 0-based */
            J--;
            globalMean += val; 
            sharderobj.preprocessing_add_edge(I, M + J, biassgd_edge_type((float)val));
        }
        sharderobj.end_preprocessing();
        
    } else {
        logstream(LOG_INFO) << "Matrix already preprocessed, just run sharder." << std::endl;
    }
    
    fclose(f);
    globalMean /= nz;
    
    logstream(LOG_INFO) << "Global mean is: " << globalMean << " Now creating shards." << std::endl;
    
    // Shard with a specified number of shards, or determine automatically if not defined
    nshards = sharderobj.execute_sharding(get_option_string("nshards", "auto"));
    
    return nshards;
}

void set_matcode(MM_typecode & matcode){
  mm_initialize_typecode(&matcode);
  mm_set_matrix(&matcode);
  mm_set_array(&matcode);
  mm_set_real(&matcode);
}






#endif