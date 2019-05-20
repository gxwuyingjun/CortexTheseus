/*!
 *  Copyright (c) 2018 by Contributors
 * \file graph_runtime_debug.cc
 */
#include <cvm/runtime/packed_func.h>
#include <cvm/runtime/registry.h>
#include <cvm/runtime/ndarray.h>
#include <chrono>
#include "../graph_runtime.h"

namespace cvm {
namespace runtime {

/*!
 * \brief Graph runtime with debug .
 *
 *  This is the extension of GraphRuntime class used for debugging
 *  CVM runtime PackedFunc API.
 */
class GraphRuntimeDebug : public GraphRuntime {
 public:
  /*!
   * \brief Run each operation and get the output.
   * \param index The index of op which needs to be run.
   * \return the elapsed time.
   */
  double DebugRun(size_t index) {
    CHECK(index < op_execs_.size());
    CVMContext ctx = data_entry_[entry_id(index, 0)]->ctx;
    auto tbegin = std::chrono::high_resolution_clock::now();
    if (op_execs_[index]) {
      op_execs_[index]();
    }
    CVMSynchronize(ctx.device_type, ctx.device_id, nullptr);
    auto tend = std::chrono::high_resolution_clock::now();
    double time = std::chrono::duration_cast<std::chrono::duration<double> >(
        tend - tbegin).count();
    return time;
  }

  /*!
   * \brief Run each operation in the graph and print out the runtime per op.
   * \param number The number of times to run this function for taking average.
   * \param repeat The number of times to repeat the measurement.
            In total, the function will be invoked (1 + number x repeat) times,
            where the first one is warmed up and will be discarded in case
            there is lazy initialization.
   * \param min_repeat_ms The minimum duration of one `repeat` in milliseconds.
            By default, one `repeat` contains `number` runs. If this parameter is set,
            the parameters `number` will be dynamically adjusted to meet the
            minimum duration requirement of one `repeat`.
   */
  void RunIndividual(int number, int repeat, int min_repeat_ms) {
    // warmup run
    GraphRuntime::Run();

    std::vector<double> time_per_op(op_execs_.size(), 0);
    for (int i = 0; i < repeat; ++i) {
      std::chrono::time_point<
        std::chrono::high_resolution_clock, std::chrono::nanoseconds> tbegin, tend;
      double duration_ms = 0.0;
      do {
        std::fill(time_per_op.begin(), time_per_op.end(), 0);
        if (duration_ms > 0.0) {
          number = static_cast<int>(
              std::max((min_repeat_ms / (duration_ms / number) + 1),
                       number * 1.618));  // 1.618 is chosen by random
        }
        tbegin = std::chrono::high_resolution_clock::now();
        for (int k = 0; k < number; k++) {
          for (size_t index = 0; index < op_execs_.size(); ++index) {
            if (op_execs_[index]) {
              const CVMContext& ctx = data_entry_[entry_id(index, 0)]->ctx;
              auto op_tbegin = std::chrono::high_resolution_clock::now();
              op_execs_[index]();
              CVMSynchronize(ctx.device_type, ctx.device_id, nullptr);
              auto op_tend = std::chrono::high_resolution_clock::now();
              double op_duration = std::chrono::duration_cast<
                  std::chrono::duration<double> >(op_tend - op_tbegin).count();
              time_per_op[index] += op_duration * 1000;  // ms
            }
          }
        }
        tend = std::chrono::high_resolution_clock::now();
        duration_ms = std::chrono::duration_cast<std::chrono::duration<double> >
            (tend - tbegin).count() * 1000;
      } while (duration_ms < min_repeat_ms);

      LOG(INFO) << "Repeat: " << i;
      int op = 0;
      for (size_t index = 0; index < time_per_op.size(); index++) {
        if (op_execs_[index]) {
          time_per_op[index] /= number;
          LOG(INFO) << "Op #" << op++ << ": " << time_per_op[index] << " ms/iter";
        }
      }
    }
  }

  /*!
   * \brief Run each operation and get the output.
   * \param index The index of op which needs to be returned.
   * \param eid The Entry id of the op.
   */
  NDArray GetOutputByLayer(int index, int eid) {
    return data_entry_[entry_id(index, eid)];
  }

  /*!
   * \brief GetFunction Get the function based on input.
   * \param name The function which needs to be invoked.
   * \param sptr_to_self Packed function pointer.
   */
  PackedFunc GetFunction(const std::string& name,
                         const std::shared_ptr<ModuleNode>& sptr_to_self);

  /*!
   * \brief Get the node index given the name of node.
   * \param name The name of the node.
   * \return The index of node.
   */
  int GetNodeIndex(const std::string& name) const {
    for (size_t nid = 0; nid < GetNumOfNodes(); ++nid) {
      if (GetNodeName(nid) == name) {
        return static_cast<int>(nid);
      }
    }
    LOG(FATAL) << "cannot find " << name << " among nodex";
    return -1;
}

/*!
 * \brief Copy index-th node to data_out.
 *
 * This method will do a partial run of the the graph
 * from begining upto the index-th node and return output of index-th node.
 * This is costly operation and suggest to use only for debug porpose.
 *
 * \param index: The  index of the node.
 * \param data_out the node data.
 */
void DebugGetNodeOutput(int index, DLTensor* data_out) {
  CHECK_LT(static_cast<size_t>(index), op_execs_.size());
  uint32_t eid = index;

  for (size_t i = 0; i < op_execs_.size(); ++i) {
    if (op_execs_[i]) op_execs_[i]();
    if (static_cast<int>(i) == index) break;
  }

  data_entry_[eid].CopyTo(data_out);
}
};


/*!
 * \brief GetFunction Get the function based on input.
 * \param name The function which needs to be invoked.
 * \param sptr_to_self Packed function pointer.
 */
PackedFunc GraphRuntimeDebug::GetFunction(
    const std::string& name,
    const std::shared_ptr<ModuleNode>& sptr_to_self) {
  // return member functions during query.
  if (name == "debug_run") {
    return PackedFunc([sptr_to_self, this](CVMArgs args, CVMRetValue* rv) {
        *rv = this->DebugRun(static_cast<size_t>(args[0].operator int64_t()));
      });
  } else if (name == "get_output_by_layer") {
    return PackedFunc([sptr_to_self, this](CVMArgs args, CVMRetValue* rv) {
        *rv = this->GetOutputByLayer(args[0], args[1]);
      });
  } else if (name == "debug_get_output") {
    return PackedFunc([sptr_to_self, this](CVMArgs args, CVMRetValue* rv) {
        if (args[0].type_code() == kStr) {
          this->DebugGetNodeOutput(this->GetNodeIndex(args[0]), args[1]);
        } else {
          this->DebugGetNodeOutput(args[0], args[1]);
        }
      });
  } else if (name == "run_individual") {
    return PackedFunc([sptr_to_self, this](CVMArgs args, CVMRetValue* rv) {
      int number = args[0];
      int repeat = args[1];
      int min_repeat_ms = args[2];
      CHECK_GT(number, 0);
      CHECK_GT(repeat, 0);
      CHECK_GE(min_repeat_ms, 0);
      this->RunIndividual(number, repeat, min_repeat_ms);
    });
  } else {
    return GraphRuntime::GetFunction(name, sptr_to_self);
  }
}

/*!
 * \brief GraphRuntimeDebugCreate Get the function based on input.
 * \param sym_json The graph symbol in json format.
 * \param m Compiled module which will be loaded.
 * \param ctxs All devices contexts.
 */
Module GraphRuntimeDebugCreate(const std::string& sym_json,
                               const cvm::runtime::Module& m,
                               const std::vector<CVMContext>& ctxs) {
  std::shared_ptr<GraphRuntimeDebug> exec = std::make_shared<GraphRuntimeDebug>();
  exec->Init(sym_json, m, ctxs);
  return Module(exec);
}

CVM_REGISTER_GLOBAL("cvm.graph_runtime_debug.create")
.set_body([](CVMArgs args, CVMRetValue* rv) {
    CHECK_GE(args.num_args, 4)
        << "The expected number of arguments for graph_runtime.create is "
           "at least 4, but it has "
        << args.num_args;
    *rv = GraphRuntimeDebugCreate(args[0], args[1], GetAllContext(args));
  });

CVM_REGISTER_GLOBAL("cvm.graph_runtime_debug.remote_create")
  .set_body([](CVMArgs args, CVMRetValue* rv) {
    CHECK_GE(args.num_args, 4) << "The expected number of arguments for "
                                  "graph_runtime.remote_create is "
                                  "at least 4, but it has "
                               << args.num_args;
    void* mhandle = args[1];
    const auto& contexts = GetAllContext(args);
    *rv = GraphRuntimeDebugCreate(
        args[0], *static_cast<cvm::runtime::Module*>(mhandle), contexts);
  });

}  // namespace runtime
}  // namespace cvm