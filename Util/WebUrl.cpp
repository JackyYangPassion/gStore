#include "WebUrl.h"

using namespace std;
string WebUrl::Request(string& url,string& request) {
	smatch result;
	if (regex_search(url.cbegin(), url.cend(), result, regex(request + "=(.*?)&"))) {
		// ƥ����ж��������url

				// *? �ظ�����Σ������������ظ�  
		return result[1];

	}
	else if (regex_search(url.cbegin(), url.cend(), result, regex(request + "=(.*)"))) {
		// ƥ��ֻ��һ��������url

		return result[1];

	}
	else {
		// �����������ƶ�����������

		return string();

	}
}