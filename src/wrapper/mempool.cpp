#include <vector>
#include "tools.hpp"
#include "wrap_helpers.hpp"
#include <cuda.hpp>
#include <mempool.hpp>
#include <boost/python/stl_iterator.hpp>




namespace py = boost::python;




namespace
{
  class device_allocator : public cuda::context_dependent
  {
    public:
      typedef CUdeviceptr pointer_type;
      typedef unsigned long size_type;

      pointer_type allocate(size_type s)
      {
        cuda::scoped_context_activation ca(get_context());

        return cuda::mem_alloc(s);
      }

      void free(pointer_type p)
      {
        cuda::scoped_context_activation ca(get_context());
        cuda::mem_free(p);
      }

      void try_release_blocks()
      {
        pycuda::run_python_gc();
      }
  };




  class host_allocator
  {
    public:
      typedef void *pointer_type;
      typedef unsigned int size_type;

      pointer_type allocate(size_type s)
      {
        return cuda::mem_alloc_host(s);
      }

      void free(pointer_type p)
      {
        cuda::mem_free_host(p);
      }

      void try_release_blocks()
      {
        pycuda::run_python_gc();
      }
  };




  template<class Allocator>
  class context_dependent_memory_pool : 
    public pycuda::memory_pool<Allocator>,
    public cuda::explicit_context_dependent
  {
    protected:
      void start_holding_blocks()
      { acquire_context(); }

      void stop_holding_blocks()
      { release_context(); }
  };




  class pooled_device_allocation 
    : public cuda::context_dependent, 
    public pycuda::pooled_allocation<context_dependent_memory_pool<device_allocator> >
  { 
    private:
      typedef 
        pycuda::pooled_allocation<context_dependent_memory_pool<device_allocator> >
        super;

    public:
      pooled_device_allocation(
          boost::shared_ptr<super::pool_type> p, super::size_type s)
        : super(p, s)
      { }

      operator CUdeviceptr()
      { return ptr(); }
  };




  pooled_device_allocation *device_pool_allocate(
      boost::shared_ptr<context_dependent_memory_pool<device_allocator> > pool,
      context_dependent_memory_pool<device_allocator>::size_type sz)
  {
    return new pooled_device_allocation(pool, sz);
  }




  PyObject *pooled_device_allocation_to_long(pooled_device_allocation const &da)
  {
    return PyLong_FromUnsignedLong(da.ptr());
  }


  
  class pooled_host_allocation 
    : public pycuda::pooled_allocation<pycuda::memory_pool<host_allocator> >
  {
    private:
      typedef 
        pycuda::pooled_allocation<pycuda::memory_pool<host_allocator> >
        super;

    public:
      pooled_host_allocation(
          boost::shared_ptr<super::pool_type> p, super::size_type s)
        : super(p, s)
      { }
  };




  py::handle<> host_pool_allocate(
      boost::shared_ptr<pycuda::memory_pool<host_allocator> > pool,
      py::object shape, py::object dtype, py::object order_py)
  {
    PyArray_Descr *tp_descr;
    if (PyArray_DescrConverter(dtype.ptr(), &tp_descr) != NPY_SUCCEED)
      throw py::error_already_set();

    std::vector<npy_intp> dims;
    std::copy(
        py::stl_input_iterator<npy_intp>(shape),
        py::stl_input_iterator<npy_intp>(),
        back_inserter(dims));

    std::auto_ptr<pooled_host_allocation> alloc(
        new pooled_host_allocation(
          pool, tp_descr->elsize*pycuda::size_from_dims(dims.size(), &dims.front())));

    NPY_ORDER order = PyArray_CORDER;
    PyArray_OrderConverter(order_py.ptr(), &order);

    int flags = 0;
    if (order == PyArray_FORTRANORDER)
      flags |= NPY_FARRAY;
    else if (order == PyArray_CORDER)
      flags |= NPY_CARRAY;
    else
      throw std::runtime_error("unrecognized order specifier");

    py::handle<> result = py::handle<>(PyArray_NewFromDescr(
        &PyArray_Type, tp_descr,
        dims.size(), &dims.front(), /*strides*/ NULL,
        alloc->ptr(), flags, /*obj*/NULL));

    py::handle<> alloc_py(handle_from_new_ptr(alloc.release()));
    PyArray_BASE(result.get()) = alloc_py.get();
    Py_INCREF(alloc_py.get());

    return result;
  }



  template<class Wrapper>
  void expose_memory_pool(Wrapper &wrapper)
  {
    typedef typename Wrapper::wrapped_type cl;
    wrapper
      .add_property("held_blocks", &cl::held_blocks)
      .add_property("active_blocks", &cl::active_blocks)
      .DEF_SIMPLE_METHOD(bin_number)
      .DEF_SIMPLE_METHOD(alloc_size)
      .DEF_SIMPLE_METHOD(free_held)
      .DEF_SIMPLE_METHOD(stop_holding)
      .staticmethod("bin_number")
      .staticmethod("alloc_size")
      ;
  }
}




void pycuda_expose_tools()
{
  py::def("bitlog2", pycuda::bitlog2);

  {
    typedef context_dependent_memory_pool<device_allocator> cl;

    py::class_<
      cl, boost::noncopyable, 
      boost::shared_ptr<cl> > wrapper("DeviceMemoryPool");
    wrapper
      .def("allocate", device_pool_allocate,
          py::return_value_policy<py::manage_new_object>())
      ;

    expose_memory_pool(wrapper);
  }

  {
    typedef pycuda::memory_pool<host_allocator> cl;

    py::class_<
      cl, boost::noncopyable, 
      boost::shared_ptr<cl> > wrapper("PageLockedMemoryPool");
    wrapper
      .def("allocate", host_pool_allocate,
          (py::arg("shape"), py::arg("dtype"), py::arg("order")="C"));
      ;

    expose_memory_pool(wrapper);
  }

  {
    typedef pooled_device_allocation cl;
    py::class_<cl, boost::noncopyable>(
        "PooledDeviceAllocation", py::no_init)
      .DEF_SIMPLE_METHOD(free)
      .def("__int__", &cl::ptr)
      .def("__long__", pooled_device_allocation_to_long)
      .def("__len__", &cl::size)
      ;

    py::implicitly_convertible<pooled_device_allocation, CUdeviceptr>();
  }

  {
    typedef pooled_host_allocation cl;
    py::class_<cl, boost::noncopyable>(
        "PooledHostAllocation", py::no_init)
      .DEF_SIMPLE_METHOD(free)
      .def("__len__", &cl::size)
      ;
  }
}
