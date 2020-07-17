# cython: cdivision = True

from pyarrow.lib cimport *
from .cpp.libstd.boost cimport std_result

cdef _convert_string_list(l):
    return [bytes(s, "utf-8") for s in l]

cdef void handle_result(std_result[void] res) except *:
    if not res.has_value():
        raise RuntimeError(res.error().message())

#
# Python Property Graph
#
cdef class PropertyGraph:
    def __init__(self, filename, node_properties=None, edge_properties=None):
        if node_properties is not None or edge_properties is not None:
            self.underlying = PropertyFileGraph.MakeWithProperties(bytes(filename, "utf-8"), _convert_string_list(node_properties), _convert_string_list(edge_properties)).value()
        else:
            self.underlying = PropertyFileGraph.Make(bytes(filename, "utf-8")).value()

    def write(self, path=None):
        if path:
            handle_result(self.underlying.get().Write(bytes(path, "utf-8")))
        else:
            handle_result(self.underlying.get().Write())

    cdef GraphTopology topology(self):
        return self.underlying.get().topology()

    cpdef uint64_t num_nodes(PropertyGraph self):
        return self.topology().num_nodes()

    def __len__(self):
        return self.num_nodes()

    cpdef uint64_t num_edges(PropertyGraph self):
        return self.topology().num_edges()

    def node_schema(self):
        return pyarrow_wrap_schema(self.underlying.get().node_schema())

    def edge_schema(self):
        return pyarrow_wrap_schema(self.underlying.get().edge_schema())

    @staticmethod
    cdef uint64_t _property_name_to_id(object prop, Schema schema) except -1:
        cdef uint64_t pid
        if isinstance(prop, str):
            try:
                pid = schema.names.index(prop)
            except ValueError:
                raise ValueError("Non-existent field name: " + prop)
        elif isinstance(prop, int):
            pid = prop
        else:
            raise TypeError("Property must be int or str")
        return pid

    def node_property_name_to_id(self, prop):
        return PropertyGraph._property_name_to_id(prop, self.node_schema())

    def edge_property_name_to_id(self, prop):
        return PropertyGraph._property_name_to_id(prop, self.edge_schema())

    def __iter__(self):
        return iter(range(self.num_nodes()))

    def edges(self, uint64_t n):
        cdef uint64_t prev
        if n == 0:
            prev = 0
        else:
            prev = self.topology().out_indices.get().Value(n-1)
        return range(prev, self.topology().out_indices.get().Value(n))

    cpdef uint64_t get_edge_dst(PropertyGraph self, uint64_t e):
        return self.topology().out_dests.get().Value(e)

    def get_node_property(self, prop):
        return pyarrow_wrap_chunked_array(self.underlying.get().NodeProperty(PropertyGraph._property_name_to_id(prop, self.node_schema())))

    def get_edge_property(self, prop):
        return pyarrow_wrap_chunked_array(self.underlying.get().EdgeProperty(PropertyGraph._property_name_to_id(prop, self.edge_schema())))

    def add_node_property(self, table):
        handle_result(self.underlying.get().AddNodeProperties(pyarrow_unwrap_table(table)))

    def add_edge_property(self, table):
        handle_result(self.underlying.get().AddEdgeProperties(pyarrow_unwrap_table(table)))

    def remove_node_property(self, prop):
        handle_result(self.underlying.get().RemoveNodeProperty(PropertyGraph._property_name_to_id(prop, self.node_schema())))

    def remove_edge_property(self, prop):
        handle_result(self.underlying.get().RemoveEdgeProperty(PropertyGraph._property_name_to_id(prop, self.edge_schema())))

    @property
    def address(self):
        return <uint64_t>self.underlying.get()

cdef api uint64_t PropertyGraph_num_nodes(PropertyFileGraph* self) nogil:
    return self.topology().num_nodes()

cdef api uint64_t PropertyGraph_num_edges(PropertyFileGraph* self) nogil:
    return self.topology().num_edges()

cdef api uint64_t PropertyGraph_edge_index(PropertyFileGraph* self, uint64_t n) nogil:
    return self.topology().out_indices.get().Value(n)

cdef api uint64_t PropertyGraph_get_edge_dst(PropertyFileGraph* self, uint64_t e) nogil:
    return self.topology().out_dests.get().Value(e)

cdef api CChunkedArray* PropertyGraph_get_node_property(PropertyFileGraph* self, uint64_t pid) nogil:
    return self.NodeProperty(pid).get()

cdef api CChunkedArray* PropertyGraph_get_edge_property(PropertyFileGraph* self, uint64_t pid) nogil:
    return self.EdgeProperty(pid).get()
