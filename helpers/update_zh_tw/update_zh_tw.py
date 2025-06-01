import codecs


#source: mumble_zh_CN_to_TW_auto_translate.ts.txt

#target: mumble_zh_TW.ts

# <message>
	# <source>Allow %1</source>
	# <translation>允許%1</translation>
# </message>

# <message>
	# <source>Blacklisted programs</source>
	# <translation type="unfinished"></translation>
# </message>

def update_translation(one_element, translation):
	pos1 = one_element.find("<translation")
	pos2 = one_element.find("</translation>")
	if pos1>=0 and pos2>0:
		one_element = one_element[:pos1] + "<translation>" + translation + "</translation>" + one_element.split("</translation>")[1]
	return one_element

def get_message_key(one_element):
	elements = one_element.split("<source>")
	if len(elements) != 2:
		return ""
	return elements[1].split("</source>")[0]
	
def get_message_value(one_element):
	elements = one_element.split("<translation>")
	if len(elements) != 2:
		return ""
	return elements[1].split("</translation>")[0]
	

def get_source_dict(source):
	elements = source.split("<message>")
	result_dict={}

	for one_element in elements[1:]:
		message_key = get_message_key(one_element)
		if message_key:
			translation = get_message_value(one_element)
			if message_key and translation:
				result_dict[message_key]=translation

	return result_dict

def process_one_element(source_dict, one_element):
	if one_element.find('type="unfinished"')>=0:
		message_key = get_message_key(one_element)
		if message_key in source_dict:
			translation = source_dict[message_key]
			one_element = update_translation(one_element, translation)

	return one_element



def main():
	f=codecs.open("mumble_zh_CN_to_TW_auto_translate.ts.txt","r", "utf-8")
	source = f.read()
	f.close()

	source_dict = get_source_dict(source)

	f=codecs.open("mumble_zh_TW.ts", "r", "utf-8")
	target = f.read()
	f.close()
	
	elements = target.split("<message>")

	print("message count: " + str(len(elements)-1))
	
	result = elements[0]

	for one_element in elements[1:]:

		one_element = process_one_element(source_dict, one_element)

		result += "<message>" + one_element


	f=codecs.open("mumble_zh_TW_updated.ts","w","utf-8")
	f.write(result)
	f.close()

main()