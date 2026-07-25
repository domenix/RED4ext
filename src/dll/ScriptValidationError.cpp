#include "ScriptValidationError.hpp"
#include "App.hpp"

// sscanf_s is an MSVC extension. The scan-set conversions below are bounded the portable way
// instead, with an explicit field width -- which is exactly what the buffer-size arguments
// were achieving, so Windows behaviour is unchanged. Both buffers are char[64].
#ifdef _MSC_VER
#pragma warning(push)
// C4996 'sscanf': every conversion below carries an explicit field width, which is exactly the
// bound sscanf_s was being used for.
#pragma warning(disable : 4996)
#endif

ValidationError ValidationError::FromString(const char* str)
{
    ValidationErrorType type = ValidationErrorType::Unknown;
    char name[64] = {0};
    char parent[64] = {0};

    if (sscanf(str, "Missing native class '%63[^']'", name) == 1)
    {
        type = ValidationErrorType::MissingClass;
    }
    else if (sscanf(str, "Missing native global function '%63[^']'", name) == 1)
    {
        type = ValidationErrorType::MissingGlobalFunction;
    }
    else if (sscanf(str, "Missing native function '%63[^']' in native class '%63[^']'", name, parent) == 2)
    {
        type = ValidationErrorType::MissingMethod;
    }
    else if (sscanf(str, "Missing native property '%63[^']' in native class '%63[^']'", name, parent) == 2)
    {
        type = ValidationErrorType::MissingProperty;
    }
    else if (sscanf(str, "Missing base class '%63[^']' of native class '%63[^']'", parent, name) == 2)
    {
        type = ValidationErrorType::MissingBaseClass;
    }
    else if (sscanf(
                 str,
                 "Native class '%63[^']' has declared base class '%63[^']' that is different than current one '%*[^']'",
                 name, parent) == 2)
    {
        type = ValidationErrorType::BaseClassMismatch;
    }
    else if (sscanf(str, "Imported property '%63[^.].%63[^']' type '%*[^']' does not match with the native one '%*[^']'",
                      parent, name) == 2)
    {
        type = ValidationErrorType::PropertyTypeMismatch;
    }

    return { .type = type, .name = name, .parent = parent };
}

std::optional<SourceRef> ValidationError::GetSourceRef() const
{
    auto& sourceRepo = App::Get()->GetScriptCompilationSystem()->GetSourceRefRepository();

    try
    {
        switch (type)
        {
        case ValidationErrorType::MissingClass:
            return sourceRepo.GetClass(name);
        case ValidationErrorType::MissingGlobalFunction:
            return sourceRepo.GetFunction(name);
        case ValidationErrorType::MissingMethod:
            return sourceRepo.GetMethod(name, parent);
        case ValidationErrorType::MissingProperty:
            return sourceRepo.GetProperty(name, parent);
        case ValidationErrorType::MissingBaseClass:
            return sourceRepo.GetClass(name);
        case ValidationErrorType::BaseClassMismatch:
            return sourceRepo.GetClass(name);
        case ValidationErrorType::PropertyTypeMismatch:
            return sourceRepo.GetProperty(name, parent);
        default:
            return {};
        }
    }
    catch (std::out_of_range)
    {
        return {};
    }
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif
