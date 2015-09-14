FROM php:5.4-fpm
# Install modules
COPY ./config/Twig/ext/twig /tmp/twig
RUN apt-get update && apt-get install -y \
        libfreetype6-dev \
        libjpeg62-turbo-dev \
        libmcrypt-dev \
        libpng12-dev \
        libcurl4-openssl-dev  \
	libxml2-dev \
    && docker-php-ext-install iconv mcrypt \
    && docker-php-ext-install gd pdo_mysql mysqli curl  \
    && docker-php-ext-install mbstring json xml pdo pdo_mysql dom hash session tokenizer
RUN mkdir -p /usr/local/etc/php
COPY ./config/php.ini /usr/local/etc/php/php.ini

#Install Twig Extenstion
COPY ./config/Twig/ext/twig /tmp/twig
RUN cd /tmp/twig && phpize \
&& ./configure && make && make install \
&& echo "extension=twig.so #For Unix systems" > /usr/local/etc/php/conf.d/docker-php-ext-twig.ini \
&& cp /tmp/twig/modules/twig.so $(php-config --extension-dir) \
&& rm -R /tmp/twig

COPY ./config/xdebug /tmp/xdebug
RUN cd /tmp/xdebug && phpize \
&& ./configure \
&& make \
&& make install \
&& echo "zend_extension=\"/usr/local/lib/php/extensions/no-debug-non-zts-20121212/xdebug.so\" \n xdebug.remote_enable=on \n #xdebug.remote_host=127.0.0.1 \n xdebug.remote_port=9000 \n xdebug.remote_connect_back=On \n xdebug.remote_handler=dbgp \n xdebug.profiler_enable=0 \n xdebug.profiler_output_dir=\"/temp/profiledir\"" > /usr/local/etc/php/conf.d/docker-php-ext-xdebug.ini \
&& rm -R /tmp/xdebug


CMD ["php-fpm"]
