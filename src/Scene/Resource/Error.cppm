export module wescene.resource:error;
import rstd;

export namespace owe::resource
{

using namespace rstd::prelude;

enum class ResourceErrorKind
{
    MissingDefinition,
    MissingContent,
    BackendFailure,
};

struct ResourceError {
    ResourceErrorKind kind { ResourceErrorKind::BackendFailure };
    String            message;
};

} // namespace owe::resource
