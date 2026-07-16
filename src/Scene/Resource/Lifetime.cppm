export module wescene.resource_registry:lifetime;
import rstd;
import wescene.resource;

import :prepared;

export namespace owe::resource_registry
{

using namespace rstd::prelude;

struct UploadLease {
    resource::ReadyToken                ready;
    rstd::vec::Vec<PreparedBufferLease> buffers;
};

class UploadScheduler {
public:
    auto Reserve() -> resource::ReadyToken {
        ++m_next_value;
        if (m_next_value == 0) ++m_next_value;
        return resource::ReadyToken { .value = m_next_value };
    }

    bool MarkSubmitted(resource::ReadyToken ready, const PreparedResourceTable& resources) {
        if (! ready.Valid() || ready.value > m_next_value) return false;
        auto leases = resources.Leases();
        if (m_in_flight
                .insert(ready.value,
                        UploadLease {
                            .ready   = ready,
                            .buffers = rstd::move(leases.buffers),
                        })
                .is_some()) {
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

struct SubmissionLease {
    resource::CompletionToken                completion;
    u64                                      program_generation { 0 };
    rstd::vec::Vec<PreparedTextureLease>     textures;
    rstd::vec::Vec<PreparedBufferLease>      buffers;
    rstd::vec::Vec<PreparedShaderLease>      shaders;
    rstd::vec::Vec<PreparedPipelineLease>    pipelines;
    rstd::vec::Vec<PreparedRenderPassLease>  render_passes;
    rstd::vec::Vec<PreparedFramebufferLease> framebuffers;
    rstd::vec::Vec<PreparedDescriptorLease>  descriptors;
    rstd::vec::Vec<PreparedExternalLease>    externals;
};

class SubmissionTracker {
public:
    auto Begin(const PreparedResourceTable& resources) -> resource::CompletionToken {
        ++m_next_value;
        if (m_next_value == 0) ++m_next_value;
        resource::CompletionToken completion { .value = m_next_value };
        auto                      leases = resources.Leases();
        if (m_in_flight
                .insert(completion.value,
                        SubmissionLease {
                            .completion         = completion,
                            .program_generation = resources.Generation(),
                            .textures           = rstd::move(leases.textures),
                            .buffers            = rstd::move(leases.buffers),
                            .shaders            = rstd::move(leases.shaders),
                            .pipelines          = rstd::move(leases.pipelines),
                            .render_passes      = rstd::move(leases.render_passes),
                            .framebuffers       = rstd::move(leases.framebuffers),
                            .descriptors        = rstd::move(leases.descriptors),
                            .externals          = rstd::move(leases.externals),
                        })
                .is_some()) {
            return {};
        }
        return completion;
    }

    auto Complete(resource::CompletionToken completion) -> Option<SubmissionLease> {
        return m_in_flight.remove(completion.value);
    }

    void Reset() {
        m_in_flight.clear();
        m_next_value = 0;
    }

    auto InFlight() const noexcept -> usize { return m_in_flight.len(); }

private:
    u64                                              m_next_value { 0 };
    rstd::collections::HashMap<u64, SubmissionLease> m_in_flight;
};

} // namespace owe::resource_registry
