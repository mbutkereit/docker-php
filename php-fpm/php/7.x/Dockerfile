FROM php:fpm
# Install modules
RUN apt-get update && apt-get install -y \
        libfreetype6-dev \
        libjpeg62-turbo-dev \
        libmcrypt-dev \
        libpng12-dev \
        libcurl4-openssl-dev  \
	libxml2-dev \
    && docker-php-ext-install iconv mcrypt \
    && docker-php-ext-configure gd --with-freetype-dir=/usr/include/ --with-jpeg-dir=/usr/include/ \
    && docker-php-ext-install gd pdo_mysql opcache mysqli curl  \
    && docker-php-ext-install mbstring json soap xml pdo pdo_mysql dom hash session tokenizer
ENV PHPREDIS_VERSION 3.0.0

RUN curl -L -o /tmp/redis.tar.gz https://github.com/phpredis/phpredis/archive/$PHPREDIS_VERSION.tar.gz \
    && tar xfz /tmp/redis.tar.gz \
    && rm -r /tmp/redis.tar.gz \
    && mkdir -p /usr/src/php/ext \
    && mv phpredis-$PHPREDIS_VERSION /usr/src/php/ext/redis \
    && docker-php-ext-install redis
RUN mkdir -p /usr/local/etc/php
COPY ./config/php.ini /usr/local/etc/php/php.ini

#Install Twig Extenstion
#COPY ./config/Twig/ext/twig /usr/src/php/ext/twig
#RUN cd /usr/src/php/ext/twig && phpize \
#&& ./configure && make && make install \
#&& echo "extension=twig.so ;For Unix systems" > /usr/local/etc/php/conf.d/docker-php-ext-twig.ini

COPY ./config/xdebug /tmp/xdebug
RUN cd /tmp/xdebug && phpize \
&& ./configure \
&& make \
&& make install \
&& echo "zend_extension=\"$(php-config --extension-dir)/xdebug.so\" \n xdebug.remote_enable=on \n ;xdebug.remote_host=127.0.0.1 \n xdebug.remote_port=9000 \n xdebug.remote_connect_back=On \n xdebug.remote_handler=dbgp \n xdebug.profiler_enable=0 \n xdebug.profiler_output_dir=\"/temp/profiledir\"" > /usr/local/etc/php/conf.d/docker-php-ext-xdebug.ini \
&& rm -R /tmp/xdebug

# For Virtualbox user
RUN usermod -u 1000 www-data

CMD ["php-fpm"]
