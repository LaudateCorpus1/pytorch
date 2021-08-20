from torch.utils.data import IterDataPipe, functional_datapipe
from typing import Callable, Iterator, List, Optional, Sized, Tuple, TypeVar, Deque
from collections import deque

T_co = TypeVar('T_co', covariant=True)


@functional_datapipe('concat')
class ConcatIterDataPipe(IterDataPipe):
    r""" :class:`ConcatIterDataPipe`.

    Iterable DataPipe to concatenate multiple Iterable DataPipes.
    args:
        datapipes: Iterable DataPipes being concatenated
    """
    datapipes: Tuple[IterDataPipe]
    length: Optional[int]

    def __init__(self, *datapipes: IterDataPipe):
        if len(datapipes) == 0:
            raise ValueError("Expected at least one DataPipe, but got nothing")
        if not all(isinstance(dp, IterDataPipe) for dp in datapipes):
            raise TypeError("Expected all inputs to be `IterDataPipe`")
        self.datapipes = datapipes  # type: ignore[assignment]
        self.length = None

    def __iter__(self) -> Iterator:
        for dp in self.datapipes:
            for data in dp:
                yield data

    def __len__(self) -> int:
        if self.length is not None:
            if self.length == -1:
                raise TypeError("{} instance doesn't have valid length".format(type(self).__name__))
            return self.length
        if all(isinstance(dp, Sized) for dp in self.datapipes):
            self.length = sum(len(dp) for dp in self.datapipes)
        else:
            self.length = -1
        return len(self)


# This is fake class to show API, going to be replaced by the copy from torchdata
# TODO(VitalyFedyunin): Replace with valid version, documentation and tests
class IterateBuffer(IterDataPipe):

    def __init__(self, buffer):
        self.buffer = buffer

    def __iter__(self):
        for i in self.buffer:
            yield i


@functional_datapipe('fork')
class ForkIterDataPipe(IterDataPipe):
    r""" :class:`ForkIterDataPipe`.

        Iterable DataPipe to create multiple instances of the same Iterable DataPipe.
        args:
            datapipe: Iterable DataPipe being copied
            num_instances: number of instances of the datapipe to create
            buffer_size: this restricts how far ahead the leading child DataPipe
             can read relative to the slowest child DataPipe
    """
    def __new__(cls, datapipe: IterDataPipe, num_instances: int, buffer_size: int = 1000):
        container = _ForkIterDataPipe(datapipe, num_instances, buffer_size)
        return [ChildDataPipe(container, i) for i in range(num_instances)]

    def __init__(self, *arg):
        raise Exception("__init__ called instead of __new__")


class _ForkIterDataPipe(IterDataPipe):
    r""" :class:`_ForkIterDataPipe`.

        Container to hold instance-specific information on behalf of ForkIterDataPipe. It tracks
        the state of its child DataPipes, maintains the buffer, and yields the next value
        as requested by the child DataPipes.
    """
    def __init__(self, datapipe: IterDataPipe, num_instances: int, buffer_size: int = 1000):
        self.main_datapipe = iter(datapipe)
        self.num_instances = num_instances
        self.buffer: Deque = deque()
        self.buffer_size = buffer_size
        self.child_pointers = [0] * num_instances  # Indicate the indices of the next element to get
        self.slowest_ptr = 0
        self.leading_ptr = 0
        self.end_ptr = float('inf')

    def get_next(self, instance_id):
        while self.child_pointers[instance_id] < self.end_ptr:
            if not self.buffer or self.child_pointers[instance_id] > self.leading_ptr:
                self.leading_ptr = self.child_pointers[instance_id]
                if self.leading_ptr - self.slowest_ptr > self.buffer_size:
                    raise BufferError("ForkIterDataPipe buffer overflow," +
                                      f"buffer size {self.buffer_size} is insufficient.")
                try:
                    self.buffer.append(self.main_datapipe.__next__())
                    self.child_pointers[instance_id] += 1
                    yield self.buffer[-1]
                except StopIteration:
                    self.end_ptr = self.leading_ptr
            else:  # Child pointer is slower than or equal to the leading_ptr
                buffer_index = self.child_pointers[instance_id] - self.slowest_ptr
                return_val = self.buffer[buffer_index]
                self.child_pointers[instance_id] += 1
                if self.child_pointers[instance_id] - 1 == self.slowest_ptr:
                    new_min = min(self.child_pointers)  # Can optimize by avoiding the call to min()
                    if self.slowest_ptr < new_min:
                        self.slowest_ptr = new_min
                        self.buffer.popleft()
                yield return_val


class ChildDataPipe(IterDataPipe):
    r""" :class:`ChildDataPipe`.

        Iteratable Datapipe that is a child of a main DataPipe. The instance of this class
        will pass its instance_id to get the next value from its main DataPipe.
        args:
            main_datapipe: Main DataPipe with a method 'get_next(instance_id)'
            instance_id: integer identifier of this instance
    """
    def __init__(self, main_datapipe, instance_id: int):
        get_next_op = getattr(main_datapipe, "get_next", None)
        if not callable(get_next_op):
            raise NotImplementedError("Main Datapipe must have method 'get_next' implemented.")
        self.main_data_pipe = main_datapipe
        self.instance_id = instance_id

    def __iter__(self):
        yield from self.main_data_pipe.get_next(self.instance_id)

@functional_datapipe('demux')
class DemultiplexerIterDataPipe(IterDataPipe):
    r""" :class:`DemultiplexerIterDataPipe`.

        Iterable DataPipe to split the input DataPipe into multiple child DataPipes, using the given
        classification function. A list of the child DataPipes is returned from this operation.
        args:
            datapipe: Iterable DataPipe being filtered
            num_instances: number of instances of the DataPipe to create
            classifier_fn: a function that maps values to an integer within the range [0, num_instances - 1]
            buffer_size: this defines the maximum number of inputs that the buffer can hold across all child
             DataPipes while waiting for their values to be yielded
    """
    def __new__(cls, datapipe: IterDataPipe, num_instances: int,
                classifier_fn: Callable[[T_co], int], buffer_size: int = 1000):
        container = _DemultiplexerIterDataPipe(datapipe, num_instances, classifier_fn, buffer_size)
        return [ChildDataPipe(container, i) for i in range(num_instances)]

    def __init__(self, *arg):
        raise Exception("__init__ called instead of __new__")


class _DemultiplexerIterDataPipe(IterDataPipe):
    r""" :class:`_DemultiplexerIterDataPipe`.

        Container to hold instance-specific information on behalf of DemultiplexerIterDataPipe. It tracks
        the state of its child DataPipes, maintains the buffer, classifies and yields the next correct value
        as requested by the child DataPipes.
    """

    def __init__(self, datapipe: IterDataPipe[T_co], num_instances: int,
                 classifier_fn: Callable[[T_co], int], buffer_size: int):
        self.main_datapipe = iter(datapipe)
        self.num_instances = num_instances
        self.buffer_size = buffer_size
        self.child_buffers: List[Deque[T_co]] = [deque() for _ in range(num_instances)]
        self.current_buffer_total = 0
        self.classifier_fn = classifier_fn

    def _find_next(self, instance_id: int) -> T_co:
        while True:
            value = self.main_datapipe.__next__()
            classification = self.classifier_fn(value)
            if classification > self.num_instances:
                raise ValueError(f"Output of the classification fn should be between 0 and {self.num_instances - 1}. " +
                                 f"{classification} is returned.")
            if classification == instance_id:
                return value
            self.child_buffers[classification].append(value)
            self.current_buffer_total += 1
            if self.current_buffer_total > self.buffer_size:
                raise BufferError(
                    f"DemultiplexerIterDataPipe buffer overflow, buffer size {self.buffer_size} is insufficient.")

    def get_next(self, instance_id: int):
        stop = False
        while not stop:
            if self.child_buffers[instance_id]:
                self.current_buffer_total -= 1
                yield self.child_buffers[instance_id].popleft()
            else:
                try:
                    yield self._find_next(instance_id)
                except StopIteration:
                    stop = True


@functional_datapipe('mux')
class MultiplexerIterDataPipe(IterDataPipe):

    def __init__(self, *datapipes):
        self.datapipes = datapipes

    def __iter__(self):
        iterators = [iter(x) for x in self.datapipes]
        finished = {}
        had_more = True
        while had_more:
            had_more = False
            for i in range(len(iterators)):
                if i not in finished:
                    try:
                        value = iterators[i].__next__()
                        had_more = True
                        yield value
                    except StopIteration:
                        finished[i] = 1


@functional_datapipe('zip')
class ZipIterDataPipe(IterDataPipe[Tuple[T_co]]):
    r""" :class:`ZipIterDataPipe`.

    Iterable DataPipe aggregates elements into a tuple from each of
    the input DataPipe. The output DataPipe is stopped when the
    shortest input DataPipe is exhausted.
    args:
        *datapipes: Iterable DataPipes being aggregated
    """
    datapipes: Tuple[IterDataPipe]
    length: Optional[int]

    def __init__(self, *datapipes: IterDataPipe):
        if not all(isinstance(dp, IterDataPipe) for dp in datapipes):
            raise TypeError("All inputs are required to be `IterDataPipe` "
                            "for `ZipIterDataPipe`.")
        super().__init__()
        self.datapipes = datapipes  # type: ignore[assignment]
        self.length = None

    def __iter__(self) -> Iterator[Tuple[T_co]]:
        for data in zip(*self.datapipes):
            yield data

    def __len__(self) -> int:
        if self.length is not None:
            if self.length == -1:
                raise TypeError("{} instance doesn't have valid length".format(type(self).__name__))
            return self.length
        if all(isinstance(dp, Sized) for dp in self.datapipes):
            self.length = min(len(dp) for dp in self.datapipes)
        else:
            self.length = -1
        return len(self)
