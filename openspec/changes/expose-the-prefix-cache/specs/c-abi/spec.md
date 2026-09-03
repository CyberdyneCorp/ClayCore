## ADDED Requirements

### Requirement: A host can hold and schedule the SDF prefix cache

The prefix cache that makes a cold window cost its suffix rather than the whole
edit history SHALL be reachable through the C API. Without it a host pays the
full walk on every window it has not touched — measured at 242 ms a dab on a
20,000-item layer against 2.32 ms with a compatible prefix — and the acceleration
exists only for callers who can link C++.

A cache SHALL BELONG TO WHOEVER MADE IT. It SHALL NOT be owned by a document,
implied by one, or destroyed with one: a cache is a session's policy and a
device's memory ceiling, and neither is a property of the artwork. This is the
rule the brick cache already states and it SHALL be the same rule, so a host does
not learn two ownership models for two caches.

Building a prefix SHALL be a call a host makes deliberately. It SHALL NOT happen
inside the call that begins a gesture: the build is the whole layer's cost — 2.17
seconds at 20,000 items — and a gesture begins at the moment an artist is already
waiting. A host SHALL be able to ask where a layer's boundary would fall, and
whether the layer is worth caching at all, without building anything.

A cache SHALL be bounded in bytes by the host, and a budget of zero SHALL mean
the cache is off rather than unbounded — an unbounded cache is a leak on a device
with a memory ceiling, and "off" is the safe reading of a field nobody filled in.

**The sampling a cache is built at and the sampling a gesture consumes it at
SHALL NOT be settable independently.** A cached prefix is keyed on its
resolution, so two that disagree produce no error and no acceleration — the cache
simply never hits, which is the worst of the three outcomes because it looks like
the feature not working rather than like a mistake. The resolution SHALL come
from one place for both.

A host SHALL be able to tell whether the cache actually served a gesture, rather
than inferring it from a timing. Counters SHALL distinguish a window seeded from
a cached prefix from one that fell back to evaluating the prefix itself.

#### Scenario: A cached prefix makes a cold window cheap from C
- **WHEN** a host builds a prefix for a deep layer and then begins a gesture on it through the C API with that cache
- **THEN** the first dab into a window nothing has touched costs its suffix rather than the whole history, and the counters attribute it to the cache

#### Scenario: A cache outlives nothing it should not
- **WHEN** a host destroys a document while holding a cache it built against that document
- **THEN** the cache remains valid and destroying it is the host's call, exactly as the brick cache behaves

#### Scenario: Beginning a gesture builds nothing
- **WHEN** a gesture begins with a cache that holds no prefix for the layer
- **THEN** the gesture begins without baking the layer, and every window is the full walk

#### Scenario: A host can ask before it pays
- **WHEN** a host asks where a layer's prefix boundary would fall under a policy
- **THEN** it is told, or told that the layer is not worth caching, without a prefix being built

#### Scenario: A budget of zero caches nothing
- **WHEN** a cache is given a byte budget of zero
- **THEN** it holds nothing, rather than holding an unbounded amount

#### Scenario: The gesture's resolution is the cache's resolution
- **WHEN** a host sets a sampling resolution for a gesture and a policy for the cache
- **THEN** the prefix is built and consumed at the same resolution, and there is no way to express two that disagree
