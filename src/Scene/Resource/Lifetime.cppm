export module wescene.resource_registry:lifetime;
import rstd;
import wescene.resource;

import :prepared;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct UploadLease {
    resource::ReadyToken ready;
};

class UploadScheduler {
public:
    auto Reserve() -> resource::ReadyToken {
        ++m_next_value;
        if (m_next_value == 0) ++m_next_value;
        return resource::ReadyToken { .value = m_next_value };
    }

    bool MarkSubmitted(resource::ReadyToken ready) {
        if (! ready.Valid() || ready.value > m_next_value) return false;
        if (m_in_flight.insert(ready.value, UploadLease { .ready = ready }).is_some()) {
            return false;
        }
        if (m_pending.is_none() || m_pending->value < ready.value) m_pending = Some(ready);
        return true;
    }

    auto Pending() const -> Option<resource::ReadyToken> {
        if (m_pending.is_none()) return None();
        return Some(resource::ReadyToken { .value = m_pending->value });
    }

    void CompleteThrough(u64 value) {
        m_in_flight.retain([value](const u64& key, UploadLease&) {
            return key > value;
        });
        if (m_pending.is_some() && m_pending->value <= value) m_pending = None();
    }

    void Reset() {
        m_in_flight.clear();
        m_pending    = None();
        m_next_value = 0;
    }

    auto InFlight() const noexcept -> usize { return m_in_flight.len(); }

private:
    u64                                          m_next_value { 0 };
    Option<resource::ReadyToken>                 m_pending;
    rstd::collections::HashMap<u64, UploadLease> m_in_flight;
};

class DescriptorArena {
public:
    bool Pin(resource::CompletionToken               completion,
             rstd::vec::Vec<PreparedDescriptorLease> bindings) {
        if (! completion.Valid()) return false;
        return m_in_flight.insert(completion.value, rstd::move(bindings)).is_none();
    }

    bool Release(resource::CompletionToken completion) {
        return m_in_flight.remove(completion.value).is_some();
    }

    void Reset() { m_in_flight.clear(); }

    auto InFlightSubmissions() const noexcept -> usize { return m_in_flight.len(); }

    auto InFlightBindings() const -> usize {
        usize count       = 0;
        auto  submissions = m_in_flight.values();
        for (auto bindings = submissions.next(); bindings.is_some();
             bindings      = submissions.next()) {
            count += (**bindings).len();
        }
        return count;
    }

private:
    rstd::collections::HashMap<u64, rstd::vec::Vec<PreparedDescriptorLease>> m_in_flight;
};

struct SubmissionLease {
    resource::CompletionToken            completion;
    u64                                  program_generation { 0 };
    rstd::vec::Vec<PreparedTextureLease> textures;
};

class SubmissionTracker {
public:
    auto Begin(const PreparedResourceTable& resources, DescriptorArena& descriptors)
        -> resource::CompletionToken {
        ++m_next_value;
        if (m_next_value == 0) ++m_next_value;
        resource::CompletionToken completion { .value = m_next_value };
        auto                      leases = resources.Leases();
        if (! descriptors.Pin(completion, rstd::move(leases.descriptors))) return {};
        if (m_in_flight
                .insert(completion.value,
                        SubmissionLease {
                            .completion         = completion,
                            .program_generation = resources.Generation(),
                            .textures           = rstd::move(leases.textures),
                        })
                .is_some()) {
            (void)descriptors.Release(completion);
            return {};
        }
        return completion;
    }

    auto Complete(resource::CompletionToken completion, DescriptorArena& descriptors)
        -> Option<SubmissionLease> {
        auto lease = m_in_flight.remove(completion.value);
        if (lease.is_some()) (void)descriptors.Release(completion);
        return lease;
    }

    void Reset(DescriptorArena& descriptors) {
        m_in_flight.clear();
        descriptors.Reset();
        m_next_value = 0;
    }

    auto InFlight() const noexcept -> usize { return m_in_flight.len(); }

private:
    u64                                              m_next_value { 0 };
    rstd::collections::HashMap<u64, SubmissionLease> m_in_flight;
};

} // namespace owe::resource_registry
