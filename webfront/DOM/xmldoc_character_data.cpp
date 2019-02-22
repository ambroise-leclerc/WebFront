#include "xmldoc_character_data.h"

namespace WebFront { namespace XMLDoc {

/**
 * Returns the character data of the node. 
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised when the node is readonly.
 * @exception DOMException
 */
const std::string& CharacterData::getData() const
{
    return getNodeValue();
}


/**
 * Set the character data of the node. 
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised when the node is readonly.
 * @exception DOMException
 */
void CharacterData::setData(const std::string& data)
{
    setNodeValue(data);
}

/**
 * The number of characters in the node.
 */
unsigned int CharacterData::getLength() const
{
    return nodeValue_.length();
}
    
/**
 * Extracts a range of data from the node.
 * @param[in] offset Offset of substring to extract.
 * @param[in] count The number of characters to extract.
 * @return The specified substring.
 * @exception DOMException
 *   DOMSTRING_SIZE_ERR: Raised if the specified range of text does 
 *   not fit into a <code>DOMString</code>.
 */
std::string CharacterData::substringData(unsigned int offset, unsigned int count) const
{
    try
    {
         return nodeValue_.substr(offset, count);
    }
    catch (std::out_of_range&)
    {
        throw DOMException(DOMException::DOMSTRING_SIZE_ERR, "offset out of range");
    }
}

/**
 * Appends the string to the end of the character data of the node.
 * @param[in] arg The string to append.
 * @exception DOMException
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 */
void CharacterData::appendData(const std::string& arg)
{
    if (isReadOnly())
        throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");
    nodeValue_.append(arg);
}
    

/**
 * Inserts a string at the specified offset.
 * @param[in] offset The offset at which to insert.
 * @param[in] arg The string to insert.
 * @exception DOMException
 *   INDEX_SIZE_ERR: Raised if the specified <code>offset</code> is 
 *   greater than the number of characters in <code>data</code>.
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 */
void CharacterData::insertData(unsigned int offset, const std::string& arg)
{
    if (isReadOnly())
    {
        throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");
    }
    try
    {
        nodeValue_.insert(offset, arg);
    }
    catch (std::out_of_range& )
    {
        throw DOMException(DOMException::INDEX_SIZE_ERR, "offset out of range");
    }
}

/**
 * Removes a range of characters from the node.
 * @param[in] offset The offset from which to start removing.
 * @param[in] count The number of characters to delete.
 * @exception DOMException
 *   INDEX_SIZE_ERR: Raised if the specified <code>offset</code> is 
 *   greater than the number of characters in <code>data</code>.
 *   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
 */
void CharacterData::deleteData(unsigned int offset, unsigned int count)
{
    if (isReadOnly())
        throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");
    try
    {
        nodeValue_.erase(offset, count);
    }
    catch (std::out_of_range&)
    {
        throw DOMException(DOMException::INDEX_SIZE_ERR, "offset out of range");
    }
}

/**
* Replaces the characters specified by the given string.
* @param[in] offset The offset of the first character to be replaced.
* @param[in] count The number of characters to replace.
* @param[in] arg The other string that will be copied.
* @exception DOMException
*   INDEX_SIZE_ERR: Raised if the specified <code>offset</code> is 
*   greater than the number of characters in <code>data</code>.
*   NO_MODIFICATION_ALLOWED_ERR: Raised if this node is readonly.
*/void CharacterData::replaceData(unsigned int offset, unsigned int count, const std::string& arg)
{
    if (isReadOnly())
    {
        throw DOMException(DOMException::NO_MODIFICATION_ALLOWED_ERR, "node is read only");
    }
    try
    {
        nodeValue_.replace(offset, count, arg);
    }
    catch (std::out_of_range&)
    {
        throw DOMException(DOMException::INDEX_SIZE_ERR, "offset out of range");
    }
}

}}