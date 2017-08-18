/** BFS -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Compute BFS on distributed Galois.
 *
 * @author Gurbinder Gill <gurbinder533@gmail.com>
 */

#include <iostream>
#include <limits>
#include "Galois/Galois.h"
#include "Galois/gstl.h"
#include "Lonestar/BoilerPlate.h"
#include "Galois/Runtime/CompilerHelperFunctions.h"

#include "Galois/Dist/OfflineGraph.h"
#include "Galois/Dist/hGraph.h"
#include "Galois/DistAccumulator.h"
#include "Galois/Runtime/Tracer.h"


static const char* const name = "BFS - Distributed Heterogeneous";
static const char* const desc = "BFS on Distributed Galois.";
static const char* const url = 0;

namespace cll = llvm::cl;
static cll::opt<std::string> inputFile(cll::Positional, cll::desc("<input file>"), cll::Required);
static cll::opt<unsigned int> maxIterations("maxIterations", cll::desc("Maximum iterations: Default 1024"), cll::init(1024));
static cll::opt<unsigned int> src_node("srcNodeId", cll::desc("ID of the source node"), cll::init(0));
static cll::opt<bool> verify("verify", cll::desc("Verify ranks by printing to 'page_ranks.#hid.csv' file"), cll::init(false));


struct NodeData {
  std::atomic<unsigned long long> dist_current;
};

typedef hGraph<NodeData, void> Graph;
typedef typename Graph::GraphNode GNode;


struct InitializeGraph {
  Graph *graph;
  llvm::cl::opt<unsigned int> &local_src_node;

  InitializeGraph(llvm::cl::opt<unsigned int>& _src_node, Graph* _graph) : local_src_node(_src_node), graph(_graph){}
  void static go(Graph& _graph) {
    struct SyncerPull_0 {
      static unsigned long long extract(uint32_t node_id, const struct NodeData & node) {
#ifdef __GALOIS_HET_CUDA__
        if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
        assert (personality == CPU);
#endif
        return node.dist_current;
      }
      static void setVal (uint32_t node_id, struct NodeData & node, unsigned long long y) {
#ifdef __GALOIS_HET_CUDA__
        if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, y);
        else if (personality == CPU)
#endif
          node.dist_current = y;
      }
      typedef unsigned long long ValTy;
    };

#ifdef __GALOIS_HET_CUDA__
    if (personality == GPU_CUDA) {
      InitializeGraph_cuda(cuda_ctx);
    } else if (personality == CPU)
    #endif
    #ifdef __GALOIS_HET_CUDA__
    	if (personality == GPU_CUDA) {
    		InitializeGraph_cuda(cuda_ctx);
    	} else if (personality == CPU)
    #endif
    Galois::do_all(_graph.begin(), _graph.end(), InitializeGraph {src_node, &_graph}, Galois::loopname("InitGraph"));

    _graph.sync_pull<SyncerPull_0>("InitializeGraph");
  }

  void operator()(GNode src) const {
    NodeData& sdata = graph->getData(src);
    sdata.dist_current = std::numeric_limits<unsigned long long>::max()/4;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    if((net.ID == 0) && (src == local_src_node)){
      sdata.dist_current = 0;
    }
  }
};

struct BFS {
  Graph* graph;
  static Galois::DGAccumulator<int> DGAccumulator_accum;

  BFS(Graph* _graph) : graph(_graph){}
  void static go(Graph& _graph){
    unsigned iteration = 0;
    do{
      DGAccumulator_accum.reset();

      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		BFS_cuda(cuda_ctx);
      	} else if (personality == CPU)
      #endif
      	struct Syncer_0 {
      		static unsigned long long extract(uint32_t node_id, const struct NodeData & node) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
      			assert (personality == CPU);
      		#endif
      			return node.dist_current;
      		}
      		static void reduce (uint32_t node_id, struct NodeData & node, unsigned long long y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) add_node_dist_current_cuda(cuda_ctx, node_id, y);
      			else if (personality == CPU)
      		#endif
      				{ Galois::atomicMin(node.dist_current, y);}
      		}
      		static void reset (uint32_t node_id, struct NodeData & node ) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, 0);
      			else if (personality == CPU)
      		#endif
      				{node.dist_current = std::numeric_limits<unsigned long long>::max()/4; }
      		}
      		typedef unsigned long long ValTy;
      	};
      	struct SyncerPull_0 {
      		static unsigned long long extract(uint32_t node_id, const struct NodeData & node) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) return get_node_dist_current_cuda(cuda_ctx, node_id);
      			assert (personality == CPU);
      		#endif
      			return node.dist_current;
      		}
      		static void setVal (uint32_t node_id, struct NodeData & node, unsigned long long y) {
      		#ifdef __GALOIS_HET_CUDA__
      			if (personality == GPU_CUDA) set_node_dist_current_cuda(cuda_ctx, node_id, y);
      			else if (personality == CPU)
      		#endif
      				node.dist_current = y;
      		}
      		typedef unsigned long long ValTy;
      	};
      #ifdef __GALOIS_HET_CUDA__
      	if (personality == GPU_CUDA) {
      		BFS_cuda(cuda_ctx);
      	} else if (personality == CPU)
      #endif
      Galois::do_all(_graph.begin(), _graph.end(), BFS { &_graph }, Galois::loopname("BFS"), Galois::write_set("sync_push", "this->graph", "struct NodeData &", "struct NodeData &" , "dist_current", "unsigned long long" , "{ Galois::atomicMin(node.dist_current, y);}",  "{node.dist_current = std::numeric_limits<unsigned long long>::max()/4; }"), Galois::write_set("sync_pull", "this->graph", "struct NodeData &", "struct NodeData &", "dist_current" , "unsigned long long"));
      _graph.sync_push<Syncer_0>("BFS");

      _graph.sync_pull<SyncerPull_0>("BFS");

     ++iteration;
     if(iteration >= maxIterations){
        DGAccumulator_accum.reset();
     }
    }while(DGAccumulator_accum.reduce());

    std::cout << " Total iteration run : " << iteration << "\n";
  }

  void operator()(GNode src) const {
    NodeData& snode = graph->getData(src);
    auto& sdist = snode.dist_current;

    for (auto jj = graph->edge_begin(src), ej = graph->edge_end(src); jj != ej; ++jj) {
      GNode dst = graph->getEdgeDst(jj);
      auto& dnode = graph->getData(dst);
      unsigned long long new_dist = 1 + sdist;
      auto old_dist = Galois::atomicMin(dnode.dist_current, new_dist);
      if(old_dist > new_dist){
        DGAccumulator_accum += 1;
      }
    }
  }
};
Galois::DGAccumulator<int>  BFS::DGAccumulator_accum;

int main(int argc, char** argv) {
  try {
    LonestarStart(argc, argv, name, desc, url);
    Galois::StatManager statManager;
    auto& net = Galois::Runtime::getSystemNetworkInterface();
    Galois::StatTimer StatTimer_init("TIMER_GRAPH_INIT"), StatTimer_total("TIMER_TOTAL"), StatTimer_hg_init("TIMER_HG_INIT");

    StatTimer_total.start();

    StatTimer_hg_init.start();
    Graph hg(inputFile, net.ID, net.Num);
    StatTimer_hg_init.stop();

    std::cout << "InitializeGraph::go called\n";
    StatTimer_init.start();
      InitializeGraph::go(hg);
    StatTimer_init.stop();

    // Verify
/*
    if(verify){
      if(net.ID == 0) {
        for(auto ii = hg.begin(); ii != hg.end(); ++ii) {
          std::cout << "[" << *ii << "]  " << hg.getData(*ii).dist_current << "\n";
        }
      }
    }
*/


    for(auto run = 0; run < numRuns; ++run){
      std::cout << "BFS::go run " << run << " called  on " << net.ID << "\n";
      std::string timer_str("TIMER_" + std::to_string(run));
      Galois::StatTimer StatTimer_main(timer_str.c_str());

      hg.reset_num_iter(run);

      StatTimer_main.start();
        BFS::go(hg);
      StatTimer_main.stop();

      if((run + 1) != numRuns){
        Galois::Runtime::getHostBarrier().wait();
        hg.reset_num_iter(run);
        InitializeGraph::go(hg);
      }
    }

   StatTimer_total.stop();

    if(verify){
      for(auto ii = hg.begin(); ii != hg.end(); ++ii) {
        Galois::Runtime::printOutput("% %\n", hg.getGID(*ii), hg.getData(*ii).dist_current);
      }
    }

    return 0;
  } catch(const char* c) {
    std::cerr << "Error: " << c << "\n";
      return 1;
  }
}