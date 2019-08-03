#include "xmldoc_document_type.h"

namespace WebFront { namespace XMLDoc {

    /**
 * The name of DTD (immediately following the DOCTYPE keyword.
 */
std::string DocumentType::getName() const
{
    return name_;
}


/**
 * A NamedNodeMap containing the general entities declared in the DTD.
 */
std::unique_ptr<NamedNodeMap> DocumentType::getEntities() const
{
    return nullptr;
}


/**
 * A NamedNodeMap containing the notations declared in the DTD.
 */
std::unique_ptr<NamedNodeMap> DocumentType::getNotations() const
{
    return nullptr;
}


/**
 * The public identifier of the external subset.
 */
const std::string& DocumentType::getPublicId() const
{
    return public_id_;
}


/**
 * The system identifier of the external subset.
 */
const std::string& DocumentType::getSystemId() const
{
    return system_id_;
}


/**
 * The internal subset.
 */
const std::string& DocumentType::getInternalSubset() const
{
    return internal_subset_;
}

}}