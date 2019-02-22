#include "xmldoc_dom_implementation.h"
#include "xmldoc_document_type.h"
#include "xmldoc_document.h"
#include <algorithm>


class WebFront::XMLDoc::DOMImplementation::Impl
{
public:
    bool hasFeature(std::string feature_name, const std::string& version_name) const
    {
        auto feature = getFeature(feature_name);
        if (feature)
        {
            if (version_name.empty())
            {
                return true;
            }
            else
                if (feature->SupportVersion(version_name))
                {
                    return true;
                }
        }
        return false;
    }

private:
    struct Feature
    {
        std::string name_;
        std::list<std::string> supported_versions_;
        bool SupportVersion(const std::string& version) const
        {
            for (auto supported_version : supported_versions_)
            {
                if (0 == supported_version.compare(version))
                    return true;
            }
            return false;
        }
    };
    std::list<Feature> features_;

private:

    void addFeature(std::string feature_name, std::string version_name)
    {
        auto feature = getFeature(feature_name);
        if (feature)
        {
            if (!feature->SupportVersion(version_name))
            {
                feature->supported_versions_.push_back(version_name);
            }
        }
        else
        {
            Feature feat;
            feat.name_=feature_name;
            feat.supported_versions_.push_back(version_name);
            features_.push_back(feat);
        }
    }

    std::shared_ptr<Feature> getFeature(std::string feature_name) const
    {
        std::transform(feature_name.begin(), feature_name.end(), feature_name.begin(), ::tolower);
        for (auto feature : features_)
        {
            if (0 == feature.name_.compare(feature_name))
            {
                return std::make_shared<Feature>(feature);
            }
        }
        return nullptr;
    }


};

namespace WebFront { namespace XMLDoc {
    DOMImplementation::DOMImplementation() : impl_(new Impl) {}
    DOMImplementation::~DOMImplementation() {}

    /**
     * Test if the DOM implementation implements a specific feature.
     * @param feature The name of the feature to test (case-insensitive).
     * @param version Version number of the feature to test.
     * @return <code>true</code> if the feature is implemented in the 
     *   specified version, <code>false</code> otherwise.
     */
    bool DOMImplementation::hasFeature(const std::string& feature, const std::string& version) const
    {
        return impl_->hasFeature(feature, version);
    }

    /**
     * Creates an empty DocumentType node.
     */
    std::unique_ptr<DocumentType> DOMImplementation::createDocumentType(
        const std::string& qualifiedName,
        const std::string& publicId, 
        const std::string& systemId)                    const
    {
        std::unique_ptr<DocumentType> doctype(new DocumentType);
        doctype->name_=qualifiedName;
        doctype->public_id_=publicId;
        doctype->system_id_=systemId;

        return doctype;
    }

    /**
     * Creates an XML <code>Document</code> object of the specified type with 
     * its document element. HTML-only DOM implementations do not need to 
     * implement this method.
     * @param[in] namespaceURI The namespace URI of the document element to create.
     * @param[in] qualifiedName The qualified name of the document element to be 
     *   created.
     * @param doctype The type of document to be created or <code>nullptr</code>.
     *   When <code>doctype</code> is not <code>nullptr</code>, its 
     *   <code>Node.ownerDocument</code> attribute is set to the document 
     *   being created.
     * @return A new <code>Document</code> object.
     * @exception DOMException
     *   INVALID_CHARACTER_ERR: Raised if the specified qualified name 
     *   contains an illegal character.
     *   <br>NAMESPACE_ERR: Raised if the <code>qualifiedName</code> is 
     *   malformed, if the <code>qualifiedName</code> has a prefix and the 
     *   <code>namespaceURI</code> is <code>null</code>, or if the 
     *   <code>qualifiedName</code> has a prefix that is "xml" and the 
     *   <code>namespaceURI</code> is different from "
     *   http://www.w3.org/XML/1998/namespace" .
     *   <br>WRONG_DOCUMENT_ERR: Raised if <code>doctype</code> has already 
     *   been used with a different document or was created from a different 
     *   implementation.
     * @since DOM Level 2
     */
    std::shared_ptr<Document> DOMImplementation::createDocument(
        const std::string& namespaceURI, 
        const std::string& qualifiedName, 
        std::shared_ptr<DocumentType> doctype) const
    {
        std::shared_ptr<Document> new_doc(new Document);
        new_doc->namespace_uri_ = namespaceURI;
        new_doc->nodeName_ = qualifiedName;

        if (nullptr == doctype->getOwnerDocument())
        {
            doctype->ownerDocument_ = new_doc;
        }
        else
        {
            if (new_doc != doctype->getOwnerDocument())
            {
                throw DOMException(DOMException::WRONG_DOCUMENT_ERR, "Doctype has already been used with a different document");
            }
        }

        return new_doc;
        
    }

}}